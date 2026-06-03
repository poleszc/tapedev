#include "linux/blk-mq.h"
#include "linux/blkdev.h"
#include "tapedev_core.h"

static int tapedev_process_chunk(struct tapedev_section *sec, bool is_write,
				 u32 target_tape, u64 pos_on_tape,
				 u32 chunk_blocks, int *sg_idx, u32 *sg_offset)
{
	int err;

	DEBUG("queue_rq sec=%d chunk: tape=%u pos=%llu blocks=%u\n", sec->id,
	      target_tape, pos_on_tape, chunk_blocks);

	// Move the head on the correct tape.
	err = tapedev_move_to(sec, target_tape, pos_on_tape);
	if (err) {
		dev_err(&sec->device->pdev->dev,
			"seek to tape=%u pos=%llu failed: %d\n", target_tape,
			pos_on_tape, err);
		return err;
	}

	// Fill the pagetable with the fragment corresponding to this chunk.
	tapedev_fill_pgtable_from_sg(sec, sg_idx, sg_offset,
				     chunk_blocks * sec->block_size);

	if (is_write)
		err = tapedev_write(sec, chunk_blocks, 0);
	else
		err = tapedev_read(sec, chunk_blocks, 0);

	if (err) {
		dev_err(&sec->device->pdev->dev,
			"%s failed: %d at tape=%u pos=%llu blocks=%u\n",
			is_write ? "WRITE" : "READ", err, target_tape,
			pos_on_tape, chunk_blocks);

		// Head state is unknown after fail. We force w fill EJECT, TAKE, REWIND
		// on the next request.
		sec->needs_recovery = true;
	}

	return err;
}

/**
 * @brief Invoked by the block layer for each request.
 */
static blk_status_t tapedev_queue_rq(struct blk_mq_hw_ctx *hctx,
				     const struct blk_mq_queue_data *bd)
{
	struct request *rq = bd->rq;
	struct tapedev_section *sec = rq->q->queuedata;
	struct tapedev_device *dev = sec->device;
	enum req_op op = req_op(rq);
	enum dma_data_direction dma_dir;
	blk_status_t status = BLK_STS_OK;
	bool is_write = (op == REQ_OP_WRITE);
	int n_sg, n_mapped;
	u64 cur_block; // Current position in blocks from the beginning of the
	// section.
	u64 remaining_blocks; // Blocks left to process.
	u64 tape_blocks; // Number of block on a single tape;

	int sg_idx = 0;
	u32 sg_offset = 0;
	int err;

	blk_mq_start_request(rq);
	// Lock so no one invokes eject tape during this request, etc.
	mutex_lock(&sec->io_mutex);

	DEBUG("queue_rq sec=%d op=%u pos=%llu bytes=%u\n", sec->id, req_op(rq),
	      (u64)blk_rq_pos(rq), blk_rq_bytes(rq));

	// We hanldle only reads and writes.
	if (op != REQ_OP_READ && op != REQ_OP_WRITE) {
		DEBUG("queue_rq sec=%d unsupported op=%u\n", sec->id, op);
		status = BLK_STS_NOTSUPP;
		goto end_no_unmap;
	}

	dma_dir = is_write ? DMA_TO_DEVICE : DMA_FROM_DEVICE;

	DEBUG("queue_rq sec=%d %s pos=%llu bytes=%u\n", sec->id,
	      is_write ? "WRITE" : "READ", (u64)blk_rq_pos(rq),
	      blk_rq_bytes(rq));

	sg_init_table(sec->sgl, TAPEDEV_BUF_PGTABLE_ENTRIES);

	// Change the bio-vec's from request to scatterlist.
	n_sg = blk_rq_map_sg(rq, sec->sgl);
	if (n_sg <= 0) {
		// Request should have at least one segment.
		DEBUG("queue_rq sec=%d blk_rq_map_sg=%d\n", sec->id, n_sg);
		status = BLK_STS_IOERR;
		goto end_no_unmap;
	}

	// Map the sg_table to DMA.
	n_mapped = dma_map_sg(&dev->pdev->dev, sec->sgl, n_sg, dma_dir);
	if (!n_mapped) {
		dev_err(&dev->pdev->dev, "dma_map_sg failed\n");
		status = BLK_STS_IOERR;
		goto end_no_unmap;
	}

	DEBUG("queue_rq sec=%d n_sg=%d n_mapped=%d\n", sec->id, n_sg, n_mapped);

	// Map bytes to blocks.
	cur_block = ((u64)blk_rq_pos(rq) << 9) / sec->block_size;
	remaining_blocks = blk_rq_bytes(rq) / sec->block_size;
	tape_blocks = sec->tape_size / sec->block_size;

	// Perform the READ/WRITE's
	while (remaining_blocks > 0) {
		u32 target_tape = (u32)(cur_block / tape_blocks) + 1;
		u64 pos_on_tape = cur_block % tape_blocks;
		u64 blocks_to_boundry = tape_blocks - pos_on_tape;
		u32 chunk_blocks =
			(u32)min_t(u64, remaining_blocks, blocks_to_boundry);

		err = tapedev_process_chunk(sec, is_write, target_tape,
					    pos_on_tape, chunk_blocks, &sg_idx,
					    &sg_offset);
		if (err) {
			status = BLK_STS_IOERR;
			goto end;
		}

		sec->current_pos += chunk_blocks;
		cur_block += chunk_blocks;
		remaining_blocks -= chunk_blocks;
	}

end:
	dma_unmap_sg(&dev->pdev->dev, sec->sgl, n_sg, dma_dir);
end_no_unmap:
	mutex_unlock(&sec->io_mutex);
	blk_mq_end_request(rq, status);
	return BLK_STS_OK;
}

static struct blk_mq_ops tapedev_mq_ops = {
	.queue_rq = tapedev_queue_rq,
};

static const struct block_device_operations tapedev_block_ops = {
	.owner = THIS_MODULE,
	.ioctl = tapedev_ioctl,
};

// =================== SECTION INIT & DEINIT ===================

static int tapedev_init_disk(struct tapedev_section *sec)
{
	struct tapedev_device *dev = sec->device;
	u64 total_sectors;
	int err;

	struct queue_limits limits = {
		.logical_block_size = sec->block_size,
		.physical_block_size = sec->block_size,
		.io_min = sec->block_size,
		.io_opt = sec->block_size,
		// Maximum number of segments per request - 512
		.max_segments = TAPEDEV_BUF_PGTABLE_ENTRIES,
		// Maximum size of a single request. 4096 sectors * 512 = 2MB.
		.max_hw_sectors = 4096,
		.dma_alignment = sec->block_size - 1,
	};

	// Initialize the gendisk
	sec->gd = blk_mq_alloc_disk(&sec->tag_set, &limits, sec);
	if (IS_ERR(sec->gd)) {
		err = PTR_ERR(sec->gd);
		dev_err(&dev->pdev->dev, "blk_mq_alloc_disk failed: %d\n", err);
		sec->gd = NULL;
		return err;
	}

	sec->gd->major = tapedev_major;
	sec->gd->first_minor = dev->idx * TAPEDEV_MAX_SECTIONS + sec->id;
	sec->gd->minors = 1;
	sec->gd->fops = &tapedev_block_ops;
	sec->gd->private_data = sec;
	snprintf(sec->gd->disk_name, DISK_NAME_LEN, "tapedev%ds%d", dev->idx,
		 sec->id);

	total_sectors = ((u64)sec->tapes_count * sec->tape_size) >> 9;
	set_capacity(sec->gd, total_sectors);

	// Add the disk.
	err = device_add_disk(&dev->pdev->dev, sec->gd, tapedev_disk_groups);
	if (err) {
		dev_err(&dev->pdev->dev, "device_add_disk failed: %d\n", err);
		put_disk(sec->gd);
		sec->gd = NULL;
		return err;
	}

	dev_info(
		&dev->pdev->dev,
		"section %d: %u tapes, type %u, %llu B per tape, /dev/%s (%llu sectors)\n",
		sec->id, sec->tapes_count, sec->tape_type, sec->tape_size,
		sec->gd->disk_name, total_sectors);
	return 0;
}

int tapedev_init_section(struct tapedev_device *dev, int i)
{
	struct tapedev_section *sec = &dev->sections[i];
	int err;

	// Initialize general data.
	sec->device = dev;
	sec->id = i;
	mutex_init(&sec->cmd_mutex);
	mutex_init(&sec->io_mutex);
	init_waitqueue_head(&sec->cmd_wq);
	sec->cmd_done = false;
	sec->current_tape = 0;
	sec->current_pos = 0;
	sec->needs_recovery = false;

	sec->tapes_count = TDEV_SECTION_IOR(dev, i, TAPEDEV_SECT_TAPES_ADDR);
	sec->tape_type = TDEV_SECTION_IOR(dev, i, TAPEDEV_SECT_TAPE_SIZE_ADDR);
	sec->tape_size = TDEV_SECTION_SIZE(sec->tape_type);
	sec->block_size = 4096;
	TDEV_SECTION_IOW(dev, i, TAPEDEV_SECT_TAPE_BLOCKSIZE_ADDR,
			 TAPEDEV_SECT_BLOCKSIZE_4096);

	// Initialize the pgtable.
	sec->pgtable = dma_alloc_coherent(&dev->pdev->dev,
					  TAPEDEV_BUF_PGTABLE_SIZE,
					  &sec->pgtable_dma, GFP_KERNEL);
	if (!sec->pgtable)
		return -ENOMEM;

	// Scatter gather list.
	sec->sgl = kmalloc_array(TAPEDEV_BUF_PGTABLE_ENTRIES, sizeof(*sec->sgl),
				 GFP_KERNEL);
	if (!sec->sgl) {
		err = -ENOMEM;
		goto out_pgtable;
	}

	// Ensure the tape is removed after `rmmod`
	u32 actual = TDEV_SECTION_IOR(dev, i, TAPEDEV_SECT_TAPE_NO_ADDR);

	if (actual != 0) {
		dev_info(&dev->pdev->dev,
			 "section %d: tape %u from prev session, ejecting\n", i,
			 actual);
		sec->current_tape = actual;
		if (tapedev_eject_tape(sec) == 0) {
			sec->current_tape = 0;
			sec->current_pos = 0;
		}
	}

	sg_init_table(sec->sgl, TAPEDEV_BUF_PGTABLE_ENTRIES);

	// Initialize tag set.
	memset(&sec->tag_set, 0, sizeof(sec->tag_set));
	sec->tag_set.ops = &tapedev_mq_ops;
	sec->tag_set.nr_hw_queues = 1;
	// We lock on commands, so we have at most one command per section.
	sec->tag_set.queue_depth = 1;
	sec->tag_set.numa_node = NUMA_NO_NODE;
	sec->tag_set.cmd_size = 0;
	sec->tag_set.flags = BLK_MQ_F_BLOCKING;

	err = blk_mq_alloc_tag_set(&sec->tag_set);
	if (err) {
		dev_err(&dev->pdev->dev, "blk_mq_alloc_tag_set failed: %d\n",
			err);
		goto out_sgl;
	}

	err = tapedev_init_disk(sec);
	if (err)
		goto out_tagset;

	return 0;

out_tagset:
	blk_mq_free_tag_set(&sec->tag_set);
out_sgl:
	kfree(sec->sgl);
	sec->sgl = NULL;
out_pgtable:
	dma_free_coherent(&dev->pdev->dev, TAPEDEV_BUF_PGTABLE_SIZE,
			  sec->pgtable, sec->pgtable_dma);
	sec->pgtable = NULL;
	mutex_destroy(&sec->io_mutex);
	mutex_destroy(&sec->cmd_mutex);
	return err;
}

// Detach the disk. This must be done when the device is still alive so
// `del_gendisk` can perform all pending requests.
void tapedev_delete_section(struct tapedev_device *dev, int i)
{
	struct tapedev_section *sec = &dev->sections[i];

	if (sec->gd)
		del_gendisk(sec->gd);
}

// Release all resources of the section. The disk must have already been
// detached by `tapedev_delete_section` when calling this.
void tapedev_free_section(struct tapedev_device *dev, int i)
{
	struct tapedev_section *sec = &dev->sections[i];

	if (sec->gd) {
		put_disk(sec->gd);
		blk_mq_free_tag_set(&sec->tag_set);
		sec->gd = NULL;
	}

	kfree(sec->sgl);
	sec->sgl = NULL;

	if (sec->pgtable) {
		dma_free_coherent(&dev->pdev->dev, TAPEDEV_BUF_PGTABLE_SIZE,
				  sec->pgtable, sec->pgtable_dma);
		sec->pgtable = NULL;
	}

	mutex_destroy(&sec->cmd_mutex);
	mutex_destroy(&sec->io_mutex);
}
