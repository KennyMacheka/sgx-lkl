#include <assert.h>
#include <endian.h>
#include <errno.h>
#include <linux/virtio_mmio.h>
#include <string.h>
#include "enclave/enclave_oe.h"
#include "enclave/enclave_util.h"
#include "enclave/sgxlkl_t.h"
#include "enclave/ticketlock.h"
#include "lkl/virtio.h"

/*
 * Function to generate an interrupt for LKL kernel to reap the virtQ data
 */
/**Shadow Implementation
 * Access shadow dev instead
 */
static void lkl_deliver_irq(uint64_t dev_id)
{
    struct virtio_dev* dev =
        sgxlkl_enclave_state.shared_memory.virtio_console_mem;

    dev->int_status |= VIRTIO_MMIO_INT_VRING;

    lkl_trigger_irq(dev->irq);
}

/*
 * Function to add a new net device to LKL
 */
/**Shadow Implementation
 *Need to use shadow dev for accessing config_len
 *Do we pass in shadow dev or normal dev in lkl_virtio_dev_setup?
 *  Answer: we pass in the shadow dev initially, virtio.c will get the original
 */
int lkl_virtio_console_add(struct virtio_dev* console)
{
    int ret = -1;

    int mmio_size = VIRTIO_MMIO_CONFIG + console->config_len;

    ret = lkl_virtio_dev_setup(console, mmio_size, &lkl_deliver_irq);

    return ret;
}
