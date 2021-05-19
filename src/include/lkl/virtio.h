#ifndef _LKL_LIB_VIRTIO_H
#define _LKL_LIB_VIRTIO_H

#include <lkl_host.h>
#include <stdint.h>

#define container_of(ptr, type, member) \
    (type*)((char*)(ptr) - __builtin_offsetof(type, member))

struct virtio_dev;

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

/*
 * Function to setup the virtio device and acquire the irq.
 */
int lkl_virtio_dev_setup(
    struct virtio_dev* dev,
    struct virtio_dev* dev_host,
    int mmio_size,
    void* virtio_req_complete);

/*
 * Function to generate the irq for notifying the frontend driver
 * about the request completion by host/backend driver.
 */
void lkl_virtio_deliver_irq(uint8_t dev_id);

/*
 * Function to allocate memory for a shadow virtio dev
 */
struct virtio_dev* alloc_shadow_virtio_dev();

#endif //_LKL_LIB_VIRTIO_H