#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int esp_err_t;
#define ESP_OK 0
#define USB_TRANSFER_FLAG_ZERO_PACK 1
#define USB_EP_DESC_GET_MPS(ep) ((ep)->mps)
#define ESP_GOTO_ON_ERROR(expr, label, ...) do { ret = (expr); if (ret) goto label; } while (0)
typedef struct usb_transfer {
    void *context, *device_handle;
    uint8_t *data_buffer;
    size_t num_bytes;
    unsigned flags;
    uint8_t bEndpointAddress;
    void (*callback)(struct usb_transfer *);
} usb_transfer_t;
typedef struct { uint8_t bEndpointAddress; unsigned mps; } usb_ep_desc_t;
typedef struct { void *dev_hdl; } cdc_device_t;
typedef struct {
    cdc_device_t *cdc_dev;
    struct { usb_transfer_t *xfer; } notif;
    struct { usb_transfer_t *in_xfer, *out_xfer; unsigned in_mps; uint8_t *in_data_buffer_base; } data;
} usbh_cdc_port_t;
static esp_err_t usb_host_transfer_alloc(size_t length, unsigned iso, usb_transfer_t **out)
{
    (void)iso; *out = calloc(1, sizeof(**out)); assert(*out);
    (*out)->data_buffer = calloc(1, length); assert((*out)->data_buffer); return ESP_OK;
}
static void notif_xfer_cb(usb_transfer_t *transfer) { (void)transfer; }
static void in_xfer_cb(usb_transfer_t *transfer) { (void)transfer; }
static void out_xfer_cb(usb_transfer_t *transfer) { (void)transfer; }
static void _cdc_transfers_free(usbh_cdc_port_t *port)
{
    usb_transfer_t *transfers[] = {port->notif.xfer, port->data.in_xfer, port->data.out_xfer};
    for (unsigned i = 0; i < 3; ++i) if (transfers[i]) {
        free(transfers[i]->data_buffer); free(transfers[i]);
    }
}

/* ACTUAL_CDC_ALLOCATOR */

/* A USB device observes packet sizes, not the host's transfer boundaries. */
static size_t receiver_pending, receiver_frames, received_lengths[8];
static void receive_packet(size_t length, size_t mps)
{
    receiver_pending += length;
    if (length < mps) {
        assert(receiver_frames < 8);
        received_lengths[receiver_frames++] = receiver_pending;
        receiver_pending = 0;
    }
}
static void transmit_frame(usb_transfer_t *transfer, size_t length, size_t mps)
{
    for (size_t remaining = length; remaining > 0;) {
        size_t packet = remaining > mps ? mps : remaining;
        receive_packet(packet, mps); remaining -= packet;
    }
    // ESP-IDF's documented ZERO_PACK contract appends a ZLP for aligned OUT data.
    if ((transfer->flags & USB_TRANSFER_FLAG_ZERO_PACK) && length % mps == 0)
        receive_packet(0, mps);
}
int main(void)
{
    usbh_cdc_port_t port = {0};
    const usb_ep_desc_t notify = { .bEndpointAddress = 0x83, .mps = 8 };
    const usb_ep_desc_t in = { .bEndpointAddress = 0x81, .mps = 64 };
    const usb_ep_desc_t out = { .bEndpointAddress = 0x02, .mps = 64 };
    cdc_device_t device = {0}; port.cdc_dev = &device;
    assert(_cdc_transfers_allocate(&port, &notify, &in, &out, 1536, 1536) == ESP_OK);
    assert(port.notif.xfer->flags == 0 && port.data.in_xfer->flags == 0);
    size_t lengths[] = {63, 64, 65, 128, 1472, 1514};
    for (unsigned i = 0; i < 6; ++i) transmit_frame(port.data.out_xfer, lengths[i], 64);
#if CONFIG_TATER_USB_ETHERNET
    assert(receiver_frames == 6 && receiver_pending == 0);
    assert(memcmp(received_lengths, lengths, sizeof(lengths)) == 0);
#else
    // Plain CDC behavior must remain untouched. This also demonstrates the ECM failure.
    assert(port.data.out_xfer->flags == 0 && receiver_frames != 6);
#endif
    _cdc_transfers_free(&port);
    puts(CONFIG_TATER_USB_ETHERNET ? "ECM aligned frame boundaries passed" : "Plain CDC behavior preserved");
    return 0;
}
