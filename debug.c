#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/sysfs.h>

#include "pud.h"
#include "jpegenc.h"

static struct pud *kobj_to_pud(struct kobject *kobj)
{
	return dev_get_drvdata(kobj_to_dev(kobj));
}

static ssize_t pud_decoder_quality_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct pud *pud = kobj_to_pud(kobj);

	return sprintf(buf, "%d\n", pud->encoder_quality);
}

static ssize_t pud_encoder_quality_store(struct kobject *kobj, struct kobj_attribute *attr,
                             const char *buf, size_t count)
{
	struct pud *pud = kobj_to_pud(kobj);
	int val;
	if (kstrtoint(buf, 10, &val) == 0)
		pud->encoder_quality = val;
	return count;
}

static struct kobj_attribute pud_decoder_quality_attr =
	__ATTR(pud_decoder_quality, 0664, pud_decoder_quality_show, pud_encoder_quality_store);

static struct attribute *pud_attributes[] = {
	&pud_decoder_quality_attr.attr,
	NULL
};

static const struct attribute_group pud_attribute_group = {
	.attrs = pud_attributes,
};

static void __init pud_sysfs_expose_params(struct device *dev)
{
	int rc;

	rc = sysfs_create_group(&dev->kobj, &pud_attribute_group);
	if (WARN_ON(rc))
		return;
}

void pud_dbg_init(struct device *dev)
{
	pud_sysfs_expose_params(dev);
}

void pud_dbg_deinit(struct device *dev)
{
	sysfs_remove_group(&dev->kobj, &pud_attribute_group);
}
