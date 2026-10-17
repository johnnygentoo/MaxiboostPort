/* libSceUsbd (the PS4's libusb): no USB devices are attached. GUNDAM VERSUS looks for arcade
 * sticks through it; controllers reach the game through libScePad instead. The device list is
 * empty, so nothing else is reached; any other call answers "no device". An event wait sleeps for
 * its timeout (capped), so a polling thread does not spin. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <time.h>

#define USBD_ERROR_INVALID_PARAM ((int32_t)0x80240002)
#define USBD_ERROR_NO_DEVICE ((int32_t)0x80240004)

static ABI int32_t usbd_init(void) { puts("Runtime: Usbd initialized (no USB devices attached)"); return 0; }
static ABI void usbd_exit(void) {}
static void *empty_list[1];
static ABI int64_t usbd_device_list(void ***list) {
    if (!list) return USBD_ERROR_INVALID_PARAM;
    *list=empty_list; return 0;
}
static ABI void usbd_free_device_list(void **list,int32_t unref) { (void)list; (void)unref; }
static ABI int32_t usbd_no_device(void) { return USBD_ERROR_NO_DEVICE; }
static ABI void *usbd_no_transfer(int32_t packets) { (void)packets; return NULL; }
static ABI void usbd_nothing(void) {}
typedef struct { int64_t seconds, microseconds; } GuestTimeval;
static ABI int32_t usbd_handle_events(const GuestTimeval *timeout) {
    int64_t us=timeout ? timeout->seconds*1000000+timeout->microseconds : 100000;
    if (us>100000) us=100000;
    if (us>0) { struct timespec t={us/1000000,(us%1000000)*1000}; nanosleep(&t,NULL); }
    return 0;
}

static const RuntimeExport exports[]={
    {"sceUsbdInit",usbd_init}, {"sceUsbdExit",usbd_exit},
    {"sceUsbdGetDeviceList",usbd_device_list}, {"sceUsbdFreeDeviceList",usbd_free_device_list},
    {"sceUsbdHandleEventsTimeout",usbd_handle_events},
    {"sceUsbdOpen",usbd_no_device}, {"sceUsbdClose",usbd_nothing},
    {"sceUsbdCheckConnected",usbd_no_device},
    {"sceUsbdGetDeviceDescriptor",usbd_no_device}, {"sceUsbdGetConfigDescriptor",usbd_no_device},
    {"sceUsbdFreeConfigDescriptor",usbd_nothing},
    {"sceUsbdGetBusNumber",usbd_no_device}, {"sceUsbdGetDeviceAddress",usbd_no_device},
    {"sceUsbdGetMaxPacketSize",usbd_no_device},
    {"sceUsbdClaimInterface",usbd_no_device}, {"sceUsbdReleaseInterface",usbd_no_device},
    {"sceUsbdKernelDriverActive",usbd_no_device}, {"sceUsbdAttachKernelDriver",usbd_no_device},
    {"sceUsbdDetachKernelDriver",usbd_no_device},
    {"sceUsbdControlTransfer",usbd_no_device}, {"sceUsbdAllocTransfer",usbd_no_transfer},
    {"sceUsbdFillInterruptTransfer",usbd_nothing}, {"sceUsbdSubmitTransfer",usbd_no_device},
};

uintptr_t runtime_usbd_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
