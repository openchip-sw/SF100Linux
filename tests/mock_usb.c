/* Simulate two programmers, with shared interface locks across processes. */
#include <libusb-1.0/libusb.h>
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

struct mock_handle { int device; int lock; };
static int devices[2];
static int device_index(libusb_device *device) { return (int *)device - devices; }
static void trace(const char *event, int device)
{
    fprintf(stderr, "MOCK %s %d\n", event, device + 1);
    fflush(stderr);
}
int libusb_init(libusb_context **context) { *context = (void *)devices; return 0; }
void libusb_exit(libusb_context *context) { (void)context; }
int libusb_set_option(libusb_context *context, enum libusb_option option, ...)
{ (void)context; (void)option; return 0; }
ssize_t libusb_get_device_list(libusb_context *context, libusb_device ***list)
{
    (void)context;
    *list = calloc(3, sizeof(**list));
    (*list)[0] = (void *)&devices[0];
    (*list)[1] = (void *)&devices[1];
    return 2;
}
void libusb_free_device_list(libusb_device **list, int unref) { (void)unref; free(list); }
libusb_device *libusb_ref_device(libusb_device *device) { return device; }
void libusb_unref_device(libusb_device *device) { (void)device; }
int libusb_get_device_descriptor(libusb_device *device, struct libusb_device_descriptor *desc)
{
    (void)device;
    memset(desc, 0, sizeof(*desc));
    desc->idVendor = 0x0483; desc->idProduct = 0xdada;
    return 0;
}
uint8_t libusb_get_bus_number(libusb_device *device) { (void)device; return 1; }
uint8_t libusb_get_device_address(libusb_device *device) { return device_index(device) + 11; }
int libusb_open(libusb_device *device, libusb_device_handle **handle)
{
    struct mock_handle *mock = calloc(1, sizeof(*mock));
    mock->device = device_index(device); mock->lock = -1;
    *handle = (void *)mock;
    trace("open", mock->device);
    return 0;
}
int libusb_claim_interface(libusb_device_handle *handle, int interface)
{
    struct mock_handle *mock = (void *)handle;
    char path[1024];
    (void)interface;
    snprintf(path, sizeof(path), "%s/device-%d", getenv("MOCK_USB_LOCKS"), mock->device);
    mock->lock = open(path, O_CREAT | O_RDWR, 0600);
    assert(mock->lock >= 0);
    if (flock(mock->lock, LOCK_EX | LOCK_NB) != 0) {
        close(mock->lock); mock->lock = -1;
        trace("busy", mock->device);
        return LIBUSB_ERROR_BUSY;
    }
    trace("claim", mock->device);
    return 0;
}
int libusb_release_interface(libusb_device_handle *handle, int interface)
{
    struct mock_handle *mock = (void *)handle;
    (void)interface;
    assert(mock->lock >= 0);
    close(mock->lock); mock->lock = -1;
    trace("release", mock->device);
    return 0;
}
void libusb_close(libusb_device_handle *handle)
{
    struct mock_handle *mock = (void *)handle;
    assert(mock->lock == -1);
    trace("close", mock->device);
    free(mock);
}
int libusb_get_configuration(libusb_device_handle *handle, int *configuration)
{ (void)handle; *configuration = 1; return 0; }
int libusb_set_configuration(libusb_device_handle *handle, int configuration)
{
    struct mock_handle *mock = (void *)handle;
    (void)configuration;
    trace("configure", mock->device);
    return 0;
}
int libusb_control_transfer(libusb_device_handle *handle, uint8_t type, uint8_t request,
    uint16_t value, uint16_t index, unsigned char *data, uint16_t length, unsigned int timeout)
{
    struct mock_handle *mock = (void *)handle;
    (void)value; (void)index; (void)timeout;
    assert(mock->lock >= 0);
    trace("control", mock->device);
    if (request == 0x0b && getenv("MOCK_USB_HOLD"))
        usleep(500000);
    if (type & LIBUSB_ENDPOINT_IN) {
        memset(data, 0, length);
        if (request == 0x08 && length == 32)
            memcpy(data, "SF100\0\0\0V:5.5.0 ", 16);
        if (request == 0x07 && length == 3) {
            unsigned int uid = getenv("MOCK_USB_UID_BASE")
                ? strtoul(getenv("MOCK_USB_UID_BASE"), NULL, 10) + mock->device
                : 129623UL + mock->device;
            data[0] = uid >> 16; data[1] = uid >> 8; data[2] = uid;
        }
    }
    return length;
}
