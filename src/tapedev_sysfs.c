#include "tapedev_core.h"

static int tapedev_ioctl_get_info(struct tapedev_section *sec,
                                  unsigned long arg) {
  // Fill the struct and move to user space.
  struct tapedev_sect_info info;
  info.tapes = sec->tapes_count;
  info.tape_type = sec->tape_type;
  info.current_tape = sec->current_tape;

  if (copy_to_user((void __user *)arg, &info, sizeof(info)))
    return -EFAULT;
  return 0;
}

static int tapedev_ioctl_eject_tape(struct tapedev_section *sec) {
  int err = 0;

  // Wait for all active commands to complete.
  mutex_lock(&sec->io_mutex);

  err = tapedev_eject_tape(sec);
  if (err == 0 || err == -ENOMEDIUM) {
    sec->current_tape = 0;
    sec->current_pos = 0;
    mutex_unlock(&sec->io_mutex);
    return 0;
  }

  sec->current_tape = 0;
  sec->current_pos = 0;
  mutex_unlock(&sec->io_mutex);
  return err;
}

int tapedev_ioctl(struct block_device *bdev, blk_mode_t mode, unsigned int cmd,
                  unsigned long arg) {
  struct tapedev_section *sec = bdev->bd_disk->private_data;

  DEBUG("ioctl sec=%d cmd=0x%x\n", sec->id, cmd);

  switch (cmd) {
  case TAPEDEV_IOCTL_GET_INFO:
    return tapedev_ioctl_get_info(sec, arg);
  case TAPEDEV_IOCTL_EJECT_TAPE:
    return tapedev_ioctl_eject_tape(sec);
  default:
    return -ENOTTY;
  }
}

static struct tapedev_section *dev_to_section(struct device *dev) {
  struct gendisk *disk = dev_to_disk(dev);
  return disk->private_data;
}

static ssize_t tape_type_show(struct device *dev, struct device_attribute *attr,
                              char *buf) {
  struct tapedev_section *sec = dev_to_section(dev);
  return sysfs_emit(buf, "%u\n", sec->tape_type);
}

static ssize_t tapes_show(struct device *dev, struct device_attribute *attr,
                          char *buf) {
  struct tapedev_section *sec = dev_to_section(dev);
  return sysfs_emit(buf, "%u\n", sec->tapes_count);
}

static ssize_t current_tape_show(struct device *dev,
                                 struct device_attribute *attr, char *buf) {
  struct tapedev_section *sec = dev_to_section(dev);
  return sysfs_emit(buf, "%u\n", sec->current_tape);
}
static DEVICE_ATTR_RO(tape_type);
static DEVICE_ATTR_RO(tapes);
static DEVICE_ATTR_RO(current_tape);

static struct attribute *tapedev_tape_attrs[] = {
    &dev_attr_tape_type.attr,
    &dev_attr_tapes.attr,
    &dev_attr_current_tape.attr,
    NULL,
};

static const struct attribute_group tapedev_tape_group = {
    .name = "tape",
    .attrs = tapedev_tape_attrs,
};

const struct attribute_group *tapedev_disk_groups[] = {
    &tapedev_tape_group,
    NULL,
};