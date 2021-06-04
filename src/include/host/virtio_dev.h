#ifndef __VIRTIO_DEV_H__
#define __VIRTIO_DEV_H__

#include <linux/virtio_ids.h>
#include <sys/uio.h>

#define BIT(x) (1ULL << x)

#define VIRTIO_F_VERSION_1 32
#define VIRTIO_RING_F_EVENT_IDX 29
#define VIRTIO_F_IOMMU_PLATFORM 33
#define VIRTIO_F_RING_PACKED 34

#define DUMMY_DATA_SIZE 1024
#define DUMMY_REQUESTS 26

extern bool packed_ring;

struct virtio_dev;

struct virtio_blk_outhdr
{
#define LKL_DEV_BLK_TYPE_READ 0
#define LKL_DEV_BLK_TYPE_WRITE 1
#define LKL_DEV_BLK_TYPE_FLUSH 4
#define LKL_DEV_BLK_TYPE_FLUSH_OUT 5
    /* VIRTIO_BLK_T* */
    uint32_t type;
    /* io priority. */
    uint32_t ioprio;
    /* Sector (ie. 512 byte offset) */
    uint64_t sector;
};

struct virtio_blk_req_trailer
{
    uint8_t status;
};

struct virtio_blkdev_dummy_req
{
    struct virtio_blk_outhdr h;
    char data[DUMMY_DATA_SIZE];
    struct virtio_blk_req_trailer t;
};

struct virtio_req
{
    uint16_t buf_count;
    struct iovec buf[32];
    uint32_t total_len;
};

struct virtio_dev_ops
{
    int (*check_features)(struct virtio_dev* dev);
    /**
     * enqueue - queues the request for processing
     *
     * Note that the current implementation assumes that the requests are
     * processed synchronous and, as such, @virtio_req_complete must be
     * called by from this function.
     *
     * @dev - virtio device
     * @q   - queue index
     *
     * @returns a negative value if the request has not been queued for
     * processing in which case the virtio device is resposible for
     * restaring the queue processing by calling @virtio_process_queue at a
     * later time; 0 or a positive value means that the request has been
     * queued for processing
     */
    int (*enqueue)(struct virtio_dev* dev, int q, struct virtio_req* req);
    /*
     * Acquire/release a lock on the specified queue. Only implemented by
     * netdevs, all other devices have NULL acquire/release function
     * pointers.
     */
    void (*acquire_queue)(struct virtio_dev* dev, int queue_idx);
    void (*release_queue)(struct virtio_dev* dev, int queue_idx);
};

struct virtio_dev
{
    uint32_t device_id;
    uint32_t vendor_id;
    uint64_t device_features;
    _Atomic(uint32_t) device_features_sel;
    uint64_t driver_features;
    _Atomic(uint32_t) driver_features_sel;
    _Atomic(uint32_t) queue_sel;
    union {
        struct {
            struct virtq* queue;
        }split;

        struct {
            struct virtq_packed* queue;
        }packed;
    };
    uint32_t queue_notify;
    _Atomic(uint32_t) int_status;
    _Atomic(uint32_t) status;
    uint32_t config_gen;
    struct virtio_dev_ops* ops;
    int irq;
    void* config_data;
    int config_len;
    void* base;
    uint32_t virtio_mmio_id;
};

void virtio_req_complete(struct virtio_req* req, uint32_t len);
void virtio_process_queue(struct virtio_dev* dev, uint32_t qidx);
void virtio_set_queue_max_merge_len(struct virtio_dev* dev, int q, int len);

#define container_of(ptr, type, member) \
    (type*)((char*)(ptr) - __builtin_offsetof(type, member))

#endif //__VIRTIO_DEV_H__
