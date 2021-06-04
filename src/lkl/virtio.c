
/* code reused from lkl/tools/lkl/lib/virtio.c and modified
 * to run some part of virtio interface inside enclave */

#include <endian.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <lkl/iomem.h>
#include <lkl/virtio.h>
#include <lkl/posix-host.h>
#include <enclave/sgxlkl_t.h>
#include <enclave/enclave_util.h>
#include <enclave/enclave_mem.h>
#include <enclave/lthread.h>
#include <shared/virtio_ring_buff.h>
#include <shared/env.h>
#include <stdatomic.h>
#include "enclave/vio_enclave_event_channel.h"
#include <linux/virtio_mmio.h>
#include <linux/virtio_ids.h>
#include "openenclave/corelibc/oestring.h"

/**
 * Obliviousness plan:
 *  1. Convert into a thread
 *  2. Experiment updating desc q at certain time intervals with a fixed no. of descriptors
 *
 */
// from inttypes.h
#ifdef PRIxPTR
    #undef PRIxPTR
    #define PRIxPTR "lx"
#else
    #define PRIxPTR "lx"
#endif

#define VIRTIO_DEV_MAGIC 0x74726976
#define VIRTIO_DEV_VERSION 2

#define BLK_DEV_NUM_QUEUES 1
#define NET_DEV_NUM_QUEUES 2
#define CONSOLE_NUM_QUEUES 2

#define BLK_DEV_QUEUE_DEPTH 32
#define CONSOLE_QUEUE_DEPTH 32
#define NET_DEV_QUEUE_DEPTH 128

#define DUMMY_SECTOR_BEGIN 10
#define DUMMY_SECTOR_END 68
#undef BIT
#define BIT(x) (1ULL << x)

#ifdef PACKED_RING
bool packed_ring = true;
#else
bool packed_ring = false;
#endif

#ifdef DEBUG
#include <openenclave/internal/print.h>
#endif

/**
 * Obliviousness test:
 *  Redirect VIRTIO_MMIO queue operations onto shadow structure (guest-bridge)
 *  Allocate queue in host side and fill it in as necessary
 *
 */

/* Used for notifying LKL for the list of virtio devices at bootup.
 * Currently block, network and console devices are passed */
char lkl_virtio_devs[4096];

/* pointer to hold the last virtio device entry to lkl_virtio_devs.
 * Each virtio devices entries are separated by a space. */
static char *devs = lkl_virtio_devs;

/* number of virtio device registered before bootup */
static uint32_t lkl_num_virtio_boot_devs;

#define DEVICE_COUNT 32

typedef void (*lkl_virtio_dev_deliver_irq)(uint64_t dev_id);
static lkl_virtio_dev_deliver_irq virtio_deliver_irq[DEVICE_COUNT];

static struct virtio_dev* dev_hosts[DEVICE_COUNT];
static struct virtio_dev* shadow_devs[DEVICE_COUNT];  //Rename to devs, change other to dev_names?

static int dummy_index = 0;

/*
 * Used for switching between the host and shadow dev structure based
 * on the virtio_read/write request
 */
struct virtio_dev_handle
{
    struct virtio_dev* dev; //shadow structure in guest memory
    struct virtio_dev* dev_host;
};

/*
 * virtio_read_device_features: Read Device Features
 * dev : pointer to device structure
 * return the device feature
 */
static inline uint32_t virtio_read_device_features(struct virtio_dev* dev)
{
    if (dev->device_features_sel)
        return (uint32_t)(dev->device_features >> 32);

    return (uint32_t)dev->device_features;
}

/*
 * virtio_has_feature: Return whether feature bit has been set on virtio device
 * dev: pointer to device structure
 * bit: feature bit
 * return whether feature bit is set
 */
static bool virtio_has_feature(struct virtio_dev* dev, unsigned int bit)
{
    return dev->device_features & BIT(bit);
}

/*
 * virtio_read: Process read requests from virtio_mmio
 * data: virtio_dev pointer
 * offset: read request type from virtio_mmio
 * res : pointer to data to be returned
 * size: device options size
 * return: returns 0 on sucess else -LKL_EINVAL on failure.
 */
static int virtio_read(void* data, int offset, void* res, int size)
{
     /* Security Review: use handle instead of virtio_dev pointer. The handle
     * contains two virtio_dev pointers, one for the shadow structure in guest
     * memory, one for the structure in host memory: struct virtio_dev_handle
     * {
     *     struct virtio_dev *dev; //shadow structure in guest memory
     *     struct virtio_dev *dev_host;
     * }
     * dev->queue points to one or more virtq shadow structure. dev_host->queue
     * points to virtq structure(s) in host memory. The desc ring and avail ring
     * of the virtq shadow structure is also shadowed, as they should be
     * host-read-only. The used ring of the virtq shadow structure points to the
     * structure in host side directly, as it should be host-read-write and
     * guest-read-only.
     *
     * For host-read-write members of a shadow structure, perform copy-through
     * read (read from host structure & update the shadow structure). For
     * host-write-once members, read from the shadow structure only, and the
     * shadow structure init routine copy the content from the host structure.
     */
    uint32_t val = 0;
    struct virtio_dev_handle* dev_handle = (struct virtio_dev_handle*)data;
    struct virtio_dev* dev = dev_handle->dev;
    struct virtio_dev* dev_host = dev_handle->dev_host;
#ifdef DEBUG
    oe_host_printf("Virtio read %x\n", offset);
#endif
    if (offset >= VIRTIO_MMIO_CONFIG)
    {
        offset -= VIRTIO_MMIO_CONFIG;
        if (offset + size > dev->config_len)
            return -LKL_EINVAL;
        memcpy(res, dev->config_data + offset, size);
        return 0;
    }
    
    if (size != sizeof(uint32_t))
        return -LKL_EINVAL;

    switch (offset)
    {
        case VIRTIO_MMIO_MAGIC_VALUE:
            val = VIRTIO_DEV_MAGIC;
            break;
        case VIRTIO_MMIO_VERSION:
            val = VIRTIO_DEV_VERSION;
            break;
        /* Security Review: dev->device_id should be host-write-once */
        case VIRTIO_MMIO_DEVICE_ID:
            val = dev->device_id;
            break;
        /* Security Review: dev->device_id should be host-write-once */
        case VIRTIO_MMIO_VENDOR_ID:
            val = dev->vendor_id;
            break;
        /* Security Review: dev->device_features should be host-write-once */
        case VIRTIO_MMIO_DEVICE_FEATURES:
            val = virtio_read_device_features(dev);
            break;
        /* Security Review: dev->queue[dev->queue_sel].num_max should be
         * host-write-once
         */
        case VIRTIO_MMIO_QUEUE_NUM_MAX:
            val = packed_ring ? dev->packed.queue[dev->queue_sel].num_max :
                                dev->split.queue[dev->queue_sel].num_max;
            break;
        case VIRTIO_MMIO_QUEUE_READY:
            val = packed_ring ? dev->packed.queue[dev->queue_sel].ready :
                                dev->split.queue[dev->queue_sel].ready;
            break;
        /* Security Review: dev->int_status is host-read-write */
        case VIRTIO_MMIO_INTERRUPT_STATUS:
            val = dev_host->int_status;
            if (dev->int_status != val)
                dev->int_status = val;
            break;
        /* Security Review: dev->status is host-read-write */
        case VIRTIO_MMIO_STATUS:
#ifdef DEBUG
            oe_host_printf("Reading status");
#endif
            val = dev_host->status;
            if (dev->status != val)
                dev->status = val;
#ifdef DEBUG
            oe_host_printf("Read status");
#endif
            break;
        /* Security Review: dev->config_gen should be host-write-once */
        case VIRTIO_MMIO_CONFIG_GENERATION:
            val = dev->config_gen;
            break;
        default:
            return -1;
    }

    *(uint32_t*)res = val;

    return 0;
}

/*
 * virtio_write_driver_features: sets driver features.
 * dev: device structure pointer
 * val: feature bits to be configured.
 * return: NONE
 */
static inline void virtio_write_driver_features(
    struct virtio_dev* dev,
    uint32_t val)
{
    uint64_t tmp;

    if (dev->driver_features_sel)
    {
        tmp = dev->driver_features & 0xFFFFFFFF;
        dev->driver_features = tmp | (uint64_t)val << 32;
    }
    else
    {
        tmp = dev->driver_features & 0xFFFFFFFF00000000;
        dev->driver_features = tmp | val;
    }
}

/*
 * blk_check_features: check device and driver features
 * dev: device structure pointer
 * return: if device and driver features are same return 0
 */
static int blk_check_features(struct virtio_dev* dev)
{
    if (dev->driver_features == dev->device_features)
        return 0;

    return -LKL_EINVAL;
}
/* set_status : set the status flag for the device
 * dev : pointer to the device structure.
 * val : Status value to be set for device
 * returns : none
 */
static inline void set_status(struct virtio_dev* dev, uint32_t val)
{
    if ((val & LKL_VIRTIO_CONFIG_S_FEATURES_OK) &&
        (!(dev->driver_features & BIT(LKL_VIRTIO_F_VERSION_1)) ||
         !(dev->driver_features & BIT(LKL_VIRTIO_RING_F_EVENT_IDX)) ||
         blk_check_features(dev)))
        val &= ~LKL_VIRTIO_CONFIG_S_FEATURES_OK;
    dev->status = val;
}

static inline void set_ptr_low(_Atomic(uint64_t) * ptr, uint32_t val)
{
    uint64_t expected = *ptr;
    uint64_t desired;

    do
    {
        desired = (expected & 0xFFFFFFFF00000000) | val;
    } while (!atomic_compare_exchange_weak(ptr, &expected, desired));
}

static inline void set_ptr_high(_Atomic(uint64_t) * ptr, uint32_t val)
{
    uint64_t expected = *ptr;
    uint64_t desired;

    do
    {
        desired = (expected & 0xFFFFFFFF00000000) | val;
        desired = (expected & 0x00000000FFFFFFFF) | ((uint64_t)val << 32);
    } while (!atomic_compare_exchange_weak(ptr, &expected, desired));
}

static void virtio_notify_host_device(struct virtio_dev* dev, struct virtio_dev* dev_host, uint32_t qidx)
{
    // TODO BUG: without print statements program may not terminate
    // Do batching stuff here and handle the rest in thread?
    // So we don't worry about time, but worry about amount and maybe the file stuff
    // Thread would handle anything that wasn't included
    // or we may not need a thread at all

    //Potential mechanism for writing:
    //Write our garbage data, then store the old data
    //Then if ever we need to read from this place again, rewrite old data first
    //What sectors should I pick? Will need to analyse the sectors typically chosen
    //Will probably need 32 different sectors
    //THen the ordering doesn't matter and I can write into a the required sector first (but then would I have to read in the next cycle- potential use for threads)
    //Step 1: create dummy descriptors with valid sectors, hopefully picking rarely used sectors
    //Sectors 10-68 aren't used in the helloworld program. 26 sectors to pick from
    //Find out if these extra used descriptors affect the process
    //step 2: sort out caching and threading


   //TODO: See if I can modify the used descriptors in lkl_deliver_irq to only send back the real descs to the guest



    // What do I put in the dummies? Random values?
    /**Virtio blk: fill desc, and fill up rest of avail
     *  outhdr
     *      sector and type (have to allocate this somewhere, and provide a sector and type)
     *          How do I allocate this - presumably in host memory?
     *          How do I specify the sector and type
     *  actual data
     *      This can be garbage, just make a char array
     *  status
     *      Needs to be allocated
     *
     *  All 3 parameters need to be allocated
     *
     *  Where do I put them in desc and avail?
     *      continue investigating
     *
     *
     *  To calculate where to put dummy descriptors:
     *
     *  Get q->last_avail_idx
     *  From that get the ring value in avail
     *  Put descriptors in after that value
     *  add them to avail and change avail->idx accordingly
     *
     *  Example process:
     *
     *  Real Desc:         Real Avail     Avail->idx   q->last_avail_idx       Fake Desc:     Fake Avail:       Fake Avail->idx after processing... q->last_avail_idx
     *
     *  0: F                0: 0            2           0                      0: F           0: 1              10                                       10
     *  1: F                1: 3                                               1: F           1: 3
     *  2: F                                                                   2: F           2: 6
     *  3: F                                                                   3: F           3: 9
     *  4: F                                                                   4: F           4: 12
     *  5: F                                                                   5: F           5: 15
     *                                                                         6: V           6: 18
     *                                                                         7: V           7: 21
     *                                                                         8: V           8: 24
     *                                                                         9: V           9: 27
     *                                                                         10:V
     *                                                                         11:V
     *                                                                         12:V
     *                                                                         13:V
     *                                                                         14:V
     *                                                                         15:V
     *                                                                         16:V
     *                                                                         17:V
     *                                                                         18:V
     *                                                                         19:V
     *                                                                         20:V
     *                                                                         21:V
     *                                                                         22:V
     *                                                                         23:V
     *                                                                         24:V
     *                                                                         25:V
     *                                                                         26:V
     *                                                                         27:V
     *                                                                         28:V
     *                                                                         29:V
     *                                                                         30:0
     *                                                                         31:0
     *
     *
     * Two options: reset avail on the next notification, along with the last_avail_idx to be the same as the guest (minus 1 the guest value)
     *              don't reset it and also change the original avail and desc, but that won't work
     *              as the guest has its own internal avail idx tracker we can't change
     *
     *
     * Questions to consider for blkdev:
     *  1. Is it right to assume that the avail_idx is always one less than avail->idx? I.e. only one blkdev is placed at a time from the guest?
    */
#ifdef DEBUG
    if (dev->device_id == VIRTIO_ID_BLOCK)
        oe_host_printf("Notifying host. device idx: %d, vendor id: %d, qidx: %d\n",
                        dev->device_id, dev->vendor_id, qidx);
#endif
    struct virtq* q = &dev->split.queue[qidx];
    struct virtq* host_q = &dev_host->split.queue[qidx];
    __sync_synchronize();
    if (!packed_ring)
    {
        uint32_t ready_val = host_q->ready;
        host_q->ready = 0;

        for (int i = 0; i < q->num; i++)
        {
#ifdef DEBUG
            if (dev->device_id == VIRTIO_ID_BLOCK)
                oe_host_printf(
                    "desc idx: %d, addr: %lu, len: %d, flags: %d\n",
                    i,
                    q->desc[i].addr,
                    q->desc[i].len,
                    q->desc[i].flags);
#endif
            host_q->desc[i].addr = q->desc[i].addr;
            host_q->desc[i].len = q->desc[i].len;
            host_q->desc[i].next = q->desc[i].next;
            host_q->desc[i].flags = q->desc[i].flags;
        }
#ifdef DEBUG
        if (dev->device_id == VIRTIO_ID_BLOCK)
            oe_host_printf(
                "last avail idx: %d, real: %d\n",
                host_q->last_avail_idx,
                host_q->last_avail_idx & 31);
#endif
        // Change avail alloc in host_interface
        host_q->avail->flags = q->avail->flags;
        host_q->avail->idx = q->avail->idx;

        for (int i = 0; i < q->avail->idx; i++)
        {
            host_q->avail->ring[i] = q->avail->ring[i];

#ifdef DEBUG
            if (dev->device_id == VIRTIO_ID_BLOCK)
                oe_host_printf(
                    "avail idx: %d, ring: %d\n", i, q->avail->ring[i] & 31);
#endif
        }

        if (dev->device_id == VIRTIO_ID_BLOCK) //blkdev only
        {
            // Don't think I need to uncomment this
            //dev_host->split.queue[qidx].last_avail_idx = q-avail->idx-1;
            // Get index
            uint16_t idx = q->avail->idx;
            uint16_t curr_desc = q->avail->ring[(idx-1) & (q->num-1)];
            struct virtio_blkdev_dummy_req* dummy_reqs = sgxlkl_enclave_state.shared_memory.dummy_virtio_blk_reqs;
            int num_dummy_reqs = DUMMY_REQUESTS;

            //For reading, I could use any offset
            //The problem is writing. I don't want to write into any offset I'm not supposed to
            //TODO change this to wrap around up curr_desc
            for (int desc_start = curr_desc+3; desc_start+2 < q->num; desc_start += 3)
            {
                // Add 3 descriptors each, and also add to avail
                // Where do I allocate this stuff?
                // Could use malloc and allocate them in sgxlkl_run_oe.c
                // Possibly put in shared_memory and call on setup?
                // Is it valid to put it in shared memory? Could an attacker make use of this?
                oe_host_printf("desc start: %d, num: %d\n", desc_start, q->num);
                struct virtio_blkdev_dummy_req *dummy_req = &dummy_reqs[dummy_index];

                host_q->desc[desc_start].addr = (uint64_t) (&dummy_req->h);
                host_q->desc[desc_start].len = sizeof(struct virtio_blk_outhdr);
                host_q->desc[desc_start].flags = LKL_VRING_DESC_F_NEXT;
                host_q->desc[desc_start].next = desc_start+1;

                host_q->desc[desc_start+1].addr = (uint64_t) (dummy_req->data);
                host_q->desc[desc_start+1].len = DUMMY_DATA_SIZE;
                host_q->desc[desc_start+1].flags = LKL_VRING_DESC_F_NEXT;

                if (dummy_req->h.type == LKL_DEV_BLK_TYPE_WRITE)
                    host_q->desc[desc_start+1].flags += LKL_VRING_DESC_F_WRITE;

                host_q->desc[desc_start+1].next = desc_start+2;

                host_q->desc[desc_start+2].addr = (uint64_t) (&dummy_req->t);
                host_q->desc[desc_start+2].len = sizeof(struct virtio_blk_req_trailer);
                host_q->desc[desc_start+2].flags = LKL_VRING_DESC_F_WRITE;

                host_q->avail->ring[idx & (q->num-1)] = desc_start;
                host_q->avail->idx++;
                idx++;
                dummy_index = (dummy_index + 1) % num_dummy_reqs;
            }
        }

        host_q->ready = ready_val;
    }
    oe_host_printf("About to notify device\n");
    uint8_t dev_id = (uint8_t)dev->vendor_id;
    vio_enclave_notify_enclave_event (dev_id, qidx);
}

/*
 * virtio_write : Handle all write requests to the device from driver
 * data : virtio_dev structure pointer
 * offset : write command
 * res : data to be written
 * size: device options size
 * return 0 if sucess else -LKL_EINVAL
 */
static int virtio_write(void* data, int offset, void* res, int size)
{
    /* Security Review: use handle instead of virtio_dev pointer. The handle
     * contains two virtio_dev pointers, one for the shadow structure in guest
     * memory, one for the structure in host memory: struct virtio_dev_handle
     * {
     *     struct virtio_dev *dev; //shadow structure in guest memory
     *     struct virtio_dev *dev_host;
     * }
     * dev->queue points to one or more virtq shadow structure. dev_host->queue
     * points to virtq structure(s) in host memory. The desc ring and avail ring
     * of the virtq shadow structure is also shadowed, as they should be
     * host-read-only. The used ring of the virtq shadow structure points to the
     * structure in host side directly, as it should be host-read-write and
     * guest-read-only.
     *
     * For host-read-only and host-read-write members of a shadow structure,
     * perform copy-through write (write to shadow structure & to host
     * structure). virtq desc and avail ring address handling is a special case.
     */
    struct virtio_dev_handle* dev_handle = (struct virtio_dev_handle*)data;
    struct virtio_dev* dev = dev_handle->dev;
    struct virtio_dev* dev_host = dev_handle->dev_host;

    struct virtq* split_q = packed_ring ? NULL : &dev_host->split.queue[dev->queue_sel];
    struct virtq_packed* packed_q = packed_ring ? &dev->packed.queue[dev->queue_sel] : NULL;
    uint32_t val;
    int ret = 0;

#ifdef DEBUG
    oe_host_printf("Virtio write %x\n", offset);
#endif
    if (offset >= VIRTIO_MMIO_CONFIG)
    {
        offset -= VIRTIO_MMIO_CONFIG;
        /* Security Review: dev->config_data and dev->config_len should be
         * host-write-once
         */
        if (offset + size >= dev->config_len)
            return -LKL_EINVAL;
        memcpy(dev->config_data + offset, res, size);
        atomic_thread_fence(memory_order_seq_cst);
        return 0;
    }

    if (size != sizeof(uint32_t))
        return -LKL_EINVAL;

    val = *(uint32_t*)res;

    switch (offset)
    {
        /* Security Review: dev->device_features_sel should be host-read-only */
        case VIRTIO_MMIO_DEVICE_FEATURES_SEL:
            if (val > 1)
                return -LKL_EINVAL;
            dev->device_features_sel = val;
            dev_host->device_features_sel = val;
            break;
        /* Security Review: dev->driver_features_sel should be host-read-only */
        case VIRTIO_MMIO_DRIVER_FEATURES_SEL:
            if (val > 1)
                return -LKL_EINVAL;
            dev->driver_features_sel = val;
            dev_host->driver_features_sel = val;
            break;
        /* Security Review: dev->driver_features should be host-read-only */
        case VIRTIO_MMIO_DRIVER_FEATURES:
            virtio_write_driver_features(dev, val);
            virtio_write_driver_features(dev_host, val);
            break;
        /* Security Review: dev->queue_sel should be host-read-only */
        case VIRTIO_MMIO_QUEUE_SEL:
            dev->queue_sel = val;
            dev_host->queue_sel = val;
            break;
        /* Security Review: dev->queue[dev->queue_sel].num should be
         * host-read-only
         */
        case VIRTIO_MMIO_QUEUE_NUM:
            if (packed_ring)
            {
                dev->packed.queue[dev->queue_sel].num = val;
                dev_host->packed.queue[dev->queue_sel].num = val;
            }
            else
            {
                dev->split.queue[dev->queue_sel].num = val;
                dev_host->split.queue[dev->queue_sel].num = val;
            }
            break;
        /* Security Review: is dev->queue[dev->queue_sel].ready host-read-only?
         */
        case VIRTIO_MMIO_QUEUE_READY:
            if (packed_ring)
            {
                dev->packed.queue[dev->queue_sel].ready = val;
                dev_host->packed.queue[dev->queue_sel].ready = val;
            }
            else
            {
                dev->split.queue[dev->queue_sel].ready = val;
                dev_host->split.queue[dev->queue_sel].ready = val;
            }
            break;
        /* Security Review: guest virtio driver(s) writes to virtq desc ring and
         * avail ring in guest memory. In queue notify flow, we need to copy the
         * update to desc ring and avail ring in host memory.
         */
        case VIRTIO_MMIO_QUEUE_NOTIFY:
            virtio_notify_host_device(dev, dev_host, val);
            break;
        /* Security Review: dev->int_status is host-read-write */
        case VIRTIO_MMIO_INTERRUPT_ACK:
            dev->int_status = 0;
            dev_host->int_status = 0;
            break;
        /* Security Review: dev->status is host-read-write */
        case VIRTIO_MMIO_STATUS:
            set_status(dev, val);
            set_status(dev_host, val);
            break;
        /* Security Review: For Split Queue, q->desc link list
         * content should be host-read-only. The Split Queue implementaiton
         * in guest side virtio code can be affected by host side manipulation
         * of q->desc[].addr and q->desc[].next. Shadowing the desc link list
         * requires extensive changes in the Split Queue code.
         * 
         * For Packed Queue, q->desc link list content is host-read-write, but
         * the Packed Queue implementation in guest side virtio code does not
         * read q->desc[].addr in buffer management flow, instead, maintaining
         * a local copy of the info. So the host side manipulation of
         * q->desc[].addr would not be effective. The Packed Queue
         * implementation still reads q->desc[].length as the size of the data
         * writtern to the "used" buffer, by the host side. Sanity check that
         * q->desc[].length should not exceed buffer size allocated might still
         * be required.
         */
        case VIRTIO_MMIO_QUEUE_DESC_LOW:
            if (packed_ring)
                set_ptr_low((_Atomic(uint64_t)*)&packed_q->desc, val);
            else
                set_ptr_low((_Atomic(uint64_t)*)&dev->split.queue[dev->queue_sel].desc, val);
            break;
        case VIRTIO_MMIO_QUEUE_DESC_HIGH:
            if (packed_ring)
                set_ptr_high((_Atomic(uint64_t)*)&packed_q->desc, val);
            else
                set_ptr_high((_Atomic(uint64_t)*)&dev->split.queue[dev->queue_sel].desc, val);
            break;
        /* Security Review: For Split Queue, q->avail link list content should be
         * host-read-only. The Split Queue implementaiton
         * in guest side virtio code only write to the avail link list, and does
         * not read from it.As long as the implementaiton does not change, host
         * side manipulation of the avail link list won't be effective. Shadowing
         * the avail linked list has the same challenge as shadowing the desc link
         * list.
         * 
         * For Packed Queue, "avail"/"driver" points to a 32-bit driver-to-device
         * notification. The Packed Queue implementaiton in guest side only writes
         * to it.
         */
        case VIRTIO_MMIO_QUEUE_AVAIL_LOW:
            if (packed_ring)
                set_ptr_low((_Atomic(uint64_t)*)&dev_host->packed.queue[dev->queue_sel].driver, val);
            else
                set_ptr_low((_Atomic(uint64_t)*)&dev->split.queue[dev->queue_sel].avail, val);
            break;
        case VIRTIO_MMIO_QUEUE_AVAIL_HIGH:
            if (packed_ring)
               set_ptr_high((_Atomic(uint64_t)*)&dev_host->packed.queue[dev->queue_sel].driver, val);
            else
               set_ptr_high((_Atomic(uint64_t)*)&dev->split.queue[dev->queue_sel].avail, val);
            break;
        /* Security Review: For Split Queue, q->used link list content should be
         * guest-read-only. The Split Queue implementaiton in guest side virtio
         * code directly read from the link list and has sanity check for
         * unexpected value of q->used.idx and q->used[].id. It reads
         * q->used[].length as the size of the data writtern to the "used"
         * buffer, by the host side. Sanity check that q->used[].length should
         * not exceed buffer size allocated might still be required.
         * 
         * For Packed Queue, "used"/"device" points to a 32-bit device-to-driver
         * notification. Host side manipulation of "device" flag can only affect
         * functionality.
         */
        case VIRTIO_MMIO_QUEUE_USED_LOW:
            if (packed_ring)
               set_ptr_low((_Atomic(uint64_t)*)&dev_host->packed.queue[dev->queue_sel].device, val);
            else
                set_ptr_low((_Atomic(uint64_t)*)&split_q->used, val);
            break;
        case VIRTIO_MMIO_QUEUE_USED_HIGH:
             if (packed_ring)
               set_ptr_high((_Atomic(uint64_t)*)&dev_host->packed.queue[dev->queue_sel].device, val);
            else
                set_ptr_high((_Atomic(uint64_t)*)&split_q->used, val);
            break;
        default:
            ret = -1;
    }

    return ret;
}

static const struct lkl_iomem_ops virtio_ops = {
    .read = virtio_read,
    .write = virtio_write,
};

static int device_num_queues(int device_id)
{
    switch(device_id)
    {
        case VIRTIO_ID_NET:
            return NET_DEV_NUM_QUEUES;
        case VIRTIO_ID_CONSOLE:
            return CONSOLE_NUM_QUEUES;
        case VIRTIO_ID_BLOCK:
            return BLK_DEV_NUM_QUEUES;
        default:
            return 0;
    }
}

/*
 * lkl_virtio_deliver_irq : Deliver the irq request to device task
 * dev_id : Device id for which irq needs to be delivered.
 */
void lkl_virtio_deliver_irq(uint8_t dev_id)
{
    // TODO change used idx value to current - dummy descs added and see if that makes a difference
    struct virtio_dev *dev = shadow_devs[dev_id];
#ifdef DEBUG
        oe_host_printf("Notifying guest. device idx: %d, vendor id: %d, qidx: %d\n",
                   dev->device_id, dev->vendor_id, dev->queue_sel);
#endif

    // Get sgxlkl_enclave_state
    if (virtio_deliver_irq[dev_id])
        virtio_deliver_irq[dev_id](dev_id);
}

static void* copy_queue(struct virtio_dev* dev)
{
    void* vq_mem = NULL;
    struct virtq_packed* dest_packed = NULL;
    struct virtq* dest_split = NULL;
    size_t vq_size = 0;
    int num_queues = device_num_queues(dev->device_id);

    if (packed_ring)
    {
        vq_size = next_pow2(num_queues * sizeof(struct virtq_packed));
    }
    else
    {
        vq_size = next_pow2(num_queues * sizeof(struct virtq));
    }

    vq_mem = sgxlkl_host_ops.mem_alloc(vq_size);

    if (!vq_mem)
    {
       sgxlkl_error("Queue mem alloc failed\n");
       return NULL;
    }

    if (packed_ring)
        dest_packed = vq_mem;
    else
        dest_split = vq_mem;

    for (int i = 0; i < num_queues; i++)
    {
       if (packed_ring)
       {
           dest_packed[i].num_max = dev->packed.queue[i].num_max;
       }

       else
       {
           dest_split[i].num_max = dev->split.queue[i].num_max;
       }
    }
    return packed_ring ? (void *) dest_packed : (void *) dest_split;
}

static bool virtqueues_in_shared_memory(struct virtio_dev* dev)
{
    int num_queues = device_num_queues(dev->device_id);

    for (int i = 0; i < num_queues; i++)
    {
        if (packed_ring)
        {
            if((oe_is_within_enclave(&dev->packed.queue[i], sizeof(struct virtq_packed))))
                return false;
        }

        else
        {
            if((oe_is_within_enclave(&dev->split.queue[i], sizeof(struct virtq))))
                return false;
        }
    }

    return true;
}

static bool supported_device(struct virtio_dev* dev)
{
    return dev->device_id == VIRTIO_ID_NET ||
           dev->device_id == VIRTIO_ID_CONSOLE ||
           dev->device_id == VIRTIO_ID_BLOCK;
}

/*
 * Function to setup the virtio device setting
 */
int lkl_virtio_dev_setup(
    struct virtio_dev* dev,
    struct virtio_dev* dev_host,
    int mmio_size,
    void* deliver_irq_cb)
{
#ifdef DEBUG
    oe_host_printf("Setting up device %d vendor id %d\n", dev->device_id, dev->vendor_id);
#endif
    struct virtio_dev_handle* dev_handle;
    int avail = 0, num_bytes = 0, ret = 0;
    size_t dev_handle_size = next_pow2(sizeof(struct virtio_dev_handle));
    dev_handle = sgxlkl_host_ops.mem_alloc(dev_handle_size);

    if (!dev_handle)
    {
        sgxlkl_error("Failed to allocate memory for dev handle\n");
        return -1;
    }

    dev_handle->dev = dev;
    dev_handle->dev_host = dev_host;

    dev->device_id = dev_host->device_id;
    dev->vendor_id = dev_host->vendor_id;
    dev->config_gen = dev_host->config_gen;
    dev->device_features = dev_host->device_features;
    dev->config_len = dev_host->config_len;
    dev->int_status = dev_host->int_status;

    if (!supported_device(dev))
    {
       sgxlkl_error("Unsupported device, device id: %d\n", dev->device_id);
       return -1;
    }

    if (dev->config_len != 0)
    {
        dev->config_data = sgxlkl_host_ops.mem_alloc(next_pow2(dev->config_len));
        if (!dev->config_data)
        {
            sgxlkl_error("Failed to allocate memory for dev config data\n");
            return -1;
        }
        memcpy(dev->config_data, dev_host->config_data, dev->config_len);
    }

    if (packed_ring)
    {
        dev->packed.queue = copy_queue(dev_host);
        if (!dev->packed.queue)
        {
            sgxlkl_error("Failed to copy packed virtqueue into shadow structure\n");
            return -1;
        }
    }
    else
    {
        dev->split.queue = copy_queue(dev_host);
        if (!dev->split.queue)
        {
            sgxlkl_error("Failed to copy split virtqueue into shadow structure\n");
            return -1;
        }
    }

    if (!virtqueues_in_shared_memory(dev_host))
    {
        sgxlkl_error("Virtqueue arrays not in shared memory\n");
        return -1;
    }

    dev->irq = lkl_get_free_irq("virtio");
    dev_host->irq = dev->irq;
    dev_host->int_status = 0;
    dev->int_status = 0;

    if (dev->irq < 0)
        return 1;

    if (packed_ring && !virtio_has_feature(dev, VIRTIO_F_RING_PACKED))
    {
        sgxlkl_error("Device %d does not support virtio packed ring\n", dev->device_id);
        return -1;
    }

    if (dev->vendor_id >= DEVICE_COUNT)
    {
        sgxlkl_error("Too many devices. Only %d devices are supported\n", DEVICE_COUNT);
        return -1;
    }

    virtio_deliver_irq[dev->vendor_id] = deliver_irq_cb;
    dev_hosts[dev->vendor_id] = dev_host;
    shadow_devs[dev->vendor_id] = dev;
    dev->base = register_iomem(dev_handle, mmio_size, &virtio_ops);

    if (!lkl_is_running())
    {
        /* Security Review: multi-thread invocation of this function can cause
         * buffer overflow in lkl_virtio_devs[4096]
         */
        avail = sizeof(lkl_virtio_devs) - (devs - lkl_virtio_devs);
        num_bytes = oe_snprintf(
            devs,
            avail,
            " virtio_mmio.device=%d@0x%" PRIxPTR ":%d",
            mmio_size,
            (uintptr_t)dev->base,
            dev->irq);
        if (num_bytes < 0 || num_bytes >= avail)
        {
            lkl_put_irq(dev->irq, "virtio");
            unregister_iomem(dev->base);
            return -LKL_ENOMEM;
        }
        devs += num_bytes;
        /* Security Review: where is dev->virtio_mmio_id used? */
        dev->virtio_mmio_id = lkl_num_virtio_boot_devs++;
    }
    else
    {
        /* Security Review: where is this function defined? */
        ret = lkl_sys_virtio_mmio_device_add(
            (long)dev->base, mmio_size, dev->irq);
        if (ret < 0)
        {
            sgxlkl_error("Can't register mmio device\n");
            return -1;
        }
        /* Security Review: where is dev->virtio_mmio_id used? */
        dev->virtio_mmio_id = lkl_num_virtio_boot_devs + ret;
    }

    return 0;
}

/*
 * Function to allocate memory for a shadow virtio dev
 */
struct virtio_dev* alloc_shadow_virtio_dev()
{
    size_t dev_size = next_pow2(sizeof(struct virtio_dev));

    struct virtio_dev* dev = sgxlkl_host_ops.mem_alloc(dev_size);

    if (!dev)
    {
        sgxlkl_error("Shadow device alloc failed\n");
        return NULL;
    }
    return dev;
}