#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/atomic.h>
#include <linux/string.h>

static struct kobject *stats_kobj;
static atomic64_t flush_count=ATOMIC64_INIT(0);

static ssize_t flush_count_show(struct kobject *kobj,
                                struct kobj_attribute *attr, char *buf){
    return sysfs_emit(buf, "%llu\n",
                       (unsigned long long)atomic64_read(&flush_count));
}

static ssize_t notify_store(struct kobject *kobj, struct kobj_attribute *attr,
                            const char *buf, size_t count){
    if(sysfs_streq(buf, "1")) atomic64_inc(&flush_count);
    return count; // Other inputs are accepted but do not change the counter.
}

static struct kobj_attribute count_attr=
    __ATTR(flush_count, 0444, flush_count_show, NULL);
static struct kobj_attribute notify_attr=
    __ATTR(notify, 0200, NULL, notify_store);

static struct attribute *stats_attrs[]={
    &count_attr.attr,
    &notify_attr.attr,
    NULL
};

static const struct attribute_group stats_group={
    .attrs=stats_attrs
};

static int __init logfs_stats_init(void){
    int ret;
    stats_kobj=kobject_create_and_add("logfs_stats", kernel_kobj);
    if(!stats_kobj) return -ENOMEM;
    ret=sysfs_create_group(stats_kobj, &stats_group);
    if(ret<0){
        kobject_put(stats_kobj);
        return ret;
    }
    return 0;
}

static void __exit logfs_stats_exit(void){
    sysfs_remove_group(stats_kobj, &stats_group);
    kobject_put(stats_kobj);
}

module_init(logfs_stats_init);
module_exit(logfs_stats_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Assignment 3 group");
MODULE_DESCRIPTION("LogFS flush counter");
