#include "tapedev_core.h"

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("tapedev device driver");
MODULE_AUTHOR("poleszc");

// =================== GLOBAL STATE ===================
/// All active devices.
static struct tapedev_device *tapedev_devices[TAPEDEV_MAX_DEVICES];
/// Lock to protect the above.
static DEFINE_MUTEX(tapedev_devices_lock);
/// Major number assigned by register_blkdev.
int tapedev_major;

// =================== INTERRUPTS ===================
static irqreturn_t tapedev_isr(int irg, void *opaque)
{
	// Strongly inspired by `adlerdev_isr`.
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

	// Device has finished initing. Wake up someone waiting in
	// `tapedev_enable_device`.
	if (istatus & BIT(TAPEDEV_IRQ_INIT_DONE)) {
		dev->init_done = true;
		wake_up(&dev->init_wq);
		handled |= BIT(TAPEDEV_IRQ_INIT_DONE);
	}

	// Hardware error.
	if (istatus & BIT(TAPEDEV_IRQ_HW_ERROR)) {
		dev_err(&dev->pdev->dev, "hardware error from tapedev\n");
		handled |= BIT(TAPEDEV_IRQ_HW_ERROR);
	}

	// Check done/error for each section.
	for (i = 0; i < dev->section_count; i++) {
		if (istatus & (BIT(TAPEDEV_IRQ_SECT_0_DONE + i) |
			       BIT(TAPEDEV_IRQ_SECT_0_ERROR + i))) {
			DEBUG("ISR Section\n");
			struct tapedev_section *sec = &dev->sections[i];

			// Save the status for it to be handled in `tapedev_send_cmd`
			sec->cmd_status = TDEV_SECTION_IOR(
				dev, i, TAPEDEV_SECT_STATUS_ADDR);
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
static int tapedev_enable_device(struct tapedev_device *dev)
{
	u32 mask;
	int i;

	// Clear all interrupts.
	TDEV_IOW(dev, TAPEDEV_IRQ_CLEAR_ADDR, ~0u);

	// Enable TAPEDEV_IRQ_INIT_DONE and TAPEDEV_IRQ_HW_ERROR.
	mask = ~((u32)BIT(TAPEDEV_IRQ_INIT_DONE) |
		 (u32)BIT(TAPEDEV_IRQ_HW_ERROR));
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
	if (dev->section_count == 0 ||
	    dev->section_count > TAPEDEV_MAX_SECTIONS) {
		DEBUG("Invalid section count\n");
		return -ENODEV;
	}

	for (i = 0; i < dev->section_count; i++) {
		mask &= ~((u32)BIT(TAPEDEV_IRQ_SECT_0_DONE + i));
		mask &= ~((u32)BIT(TAPEDEV_IRQ_SECT_0_ERROR + i));
	}
	TDEV_IOW(dev, TAPEDEV_IRQ_MASK_ADDR, mask);
	return 0;
}

// =================== PCI DRIVER ===================
static int tapedev_alloc_idx(struct tapedev_device *dev)
{
	int i, err = -ENOSPC;
	mutex_lock(&tapedev_devices_lock);
	for (i = 0; i < TAPEDEV_MAX_DEVICES; i++) {
		if (!tapedev_devices[i]) {
			tapedev_devices[i] = dev;
			dev->idx = i;
			err = 0;
			break;
		}
	}
	mutex_unlock(&tapedev_devices_lock);
	return err;
}

static void tapedev_free_idx(struct tapedev_device *dev)
{
	mutex_lock(&tapedev_devices_lock);
	tapedev_devices[dev->idx] = 0;
	mutex_unlock(&tapedev_devices_lock);
}

static int tapedev_probe(struct pci_dev *pdev,
			 const struct pci_device_id *pci_id)
{
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

	// Allocate the free device index.
	if ((err = tapedev_alloc_idx(dev)))
		goto out_slot;

	// Enable PCI device.
	if ((err = pci_enable_device(pdev)))
		goto out_enable;

	// Set the DMA mask.
	if ((err = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(40))))
		goto out_mask;

	pci_set_master(pdev);

	if ((err = pci_request_regions(pdev, TAPEDEV_NAME)))
		goto out_regions;

	// Map the BAR.
	if (!(dev->bar = pci_iomap(pdev, 0, 0))) {
		err = -ENOMEM;
		goto out_bar;
	}

	// Connect the IRQ line.
	if ((err = request_irq(pdev->irq, tapedev_isr, IRQF_SHARED,
			       TAPEDEV_NAME, dev)))
		goto out_irq;

	// Turn on the device and wait for INIT_DONE.
	if ((err = tapedev_enable_device(dev)))
		goto out_enable_hw;

	// Initialize each section.
	for (i = 0; i < dev->section_count; i++) {
		if ((err = tapedev_init_section(dev, i))) {
			// Free all sections registered up to this point when an error occurs.
			while (--i >= 0) {
				tapedev_delete_section(dev, i);
				tapedev_free_section(dev, i);
			}
			goto out_sections;
		}
	}

	dev_info(&pdev->dev, "tapedev%d ready, %u sections\n", dev->idx,
		 dev->section_count);
	return 0;

out_sections:
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

static void tapedev_remove(struct pci_dev *pdev)
{
	// Strongly inspired by `adlerdev_remove`.
	DEBUG("tapedev_remove\n");
	struct tapedev_device *dev = pci_get_drvdata(pdev);
	int i;

	// Detach the disks first, so `del_gendisk()` can perform all penging
	// requests when the device is still on.
	for (i = 0; i < dev->section_count; i++)
		tapedev_delete_section(dev, i);

	// No block IO can be performed anymore, since we've deleted the disks.
	// Eject all tapes to ensure no desynchronization on `rmmod`/`insmod` cycles.
	for (i = 0; i < dev->section_count; i++) {
		struct tapedev_section *sec = &dev->sections[i];
		if (sec->current_tape != 0) {
			tapedev_eject_tape(sec);
			sec->current_tape = 0;
		}
	}

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
	tapedev_free_idx(dev);

	kfree(dev);
}

static struct pci_device_id tapedev_pci_ids[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_TAPEDEV, PCI_DEVICE_ID_TAPEDEV) },
	{ 0 },
};

static struct pci_driver tapedev_pci_driver = {
	.name = TAPEDEV_NAME,
	.id_table = tapedev_pci_ids,
	.probe = tapedev_probe,
	.remove = tapedev_remove,
};

// =================== MODULE INIT & EXIT ===================
static int tapedev_init(void)
{
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

static void tapedev_exit(void)
{
	pci_unregister_driver(&tapedev_pci_driver);
	unregister_blkdev(tapedev_major, TAPEDEV_NAME);
	return;
}

module_init(tapedev_init);
module_exit(tapedev_exit);
