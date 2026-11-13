//
//  IntelBTPatcher.h
//  IntelBTPatcher
//
//  Created by zxystd <zxystd@foxmail.com> on 2021/2/8.
//

#ifndef IntelBTPatcher_h
#define IntelBTPatcher_h

#include <Headers/kern_patcher.hpp>

#include <IOKit/usb/IOUSBHostDevice.h>
#include <IOKit/usb/IOUSBHostInterface.h>

#define DRV_NAME "ibtp"

class BluetoothDeviceAddress;

// One per queued read on the HCI event pipe; the caller's completion is kept in orig.
typedef struct {
    IOUSBHostCompletion orig;
    IOUSBHostCompletion hook;
    IOMemoryDescriptor *dataBuffer;
} EventReadContext;

typedef struct __attribute__((packed))
{
    uint16_t    opcode;    /* OCF & OGF */
    uint8_t     len;
    uint8_t     data[];
} HciCommandHdr;

typedef struct __attribute__((packed))
{
    uint8_t     evt;
    uint8_t     len;
} HciEventHdr;

typedef struct __attribute__((packed)) 
{
    HciEventHdr evt;
    uint8_t     numCommands;
    uint16_t    opcode;
    uint8_t     data[];
} HciResponse;

const char *requestDirectionNames[] = {
    "OUT",
    "IN"
};

const char *requestTypeNames[] = {
    "Standard",
    "Class",
    "Vendor"
};

const char *requestRecipientNames[] = {
    "Device",
    "Interface",
    "Endpoint",
    "Other"
};

const char* _hexDumpHCIData(uint8_t *buf, size_t len)
{
    ssize_t str_len = len * 3 + 1;
    char *str = (char*)IOMalloc(str_len);
    if (!str)
        return nullptr;
    for (size_t i = 0; i < len; i++)
    snprintf(str + 3 * i, (len - i) * 3, "%02x ", buf[i]);
    str[MAX(str_len - 2, 0)] = 0;
    return str;
}

class CIntelBTPatcher {
public:
    bool init();
    void free();
    
    void processKext(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size);
    static IOReturn newFindQueueRequest(void *that, unsigned short arg1, void *addr, unsigned short arg2, bool arg3, void **hciRequestPtr);
    
    static IOReturn newHostDeviceRequest(void *that, IOService *provider, StandardUSB::DeviceRequest &request, void *data, IOMemoryDescriptor *descriptor, unsigned int &length,IOUSBHostCompletion *completion, unsigned int timeout);

    static IOReturn newAsyncIO(void *that, IOMemoryDescriptor *dataBuffer, uint32_t dataBufferLength, IOUSBHostCompletion *completion, uint32_t completionTimeoutMs);

    mach_vm_address_t oldFindQueueRequest {};
    mach_vm_address_t oldHostDeviceRequest {};
    mach_vm_address_t oldAsyncIO {};

    static void * volatile _eventPipe;
    static void * volatile _eventPipeDevice;
    static bool _dlePatchEnabled;   // route IOUSBHostPipe::io
    static bool _dleLookupEnabled;  // also find the event pipe, so reads actually get patched

private:
    static bool _randomAddressInit;
};

#endif /* IntelBTPatcher_h */
