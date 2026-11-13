//
//  IntelBTPatcher.cpp
//  IntelBTPatcher
//
//  Created by zxystd <zxystd@foxmail.com> on 2021/2/8.
//

#include <Headers/kern_api.hpp>
#include <Headers/kern_util.hpp>
#include <Headers/plugin_start.hpp>
#include <kern/thread_call.h>

#include "IntelBTPatcher.hpp"

static CIntelBTPatcher ibtPatcher;
static CIntelBTPatcher *callbackIBTPatcher = nullptr;

static const char *bootargOff[] {
    "-ibtcompatoff"
};

static const char *bootargDebug[] {
    "-ibtcompatdbg"
};

static const char *bootargBeta[] {
    "-ibtcompatbeta"
};

PluginConfiguration ADDPR(config) {
    xStringify(PRODUCT_NAME),
    parseModuleVersion(xStringify(MODULE_VERSION)),
    LiluAPI::AllowNormal | LiluAPI::AllowInstallerRecovery | LiluAPI::AllowSafeMode,
    bootargOff,
    arrsize(bootargOff),
    bootargDebug,
    arrsize(bootargDebug),
    bootargBeta,
    arrsize(bootargBeta),
    KernelVersion::MountainLion,
    KernelVersion::Tahoe,
    []() {
        ibtPatcher.init();
    }
};

static const char *IntelBTPatcher_IOBluetoothFamily[] { "/System/Library/Extensions/IOBluetoothFamily.kext/Contents/MacOS/IOBluetoothFamily" };

static KernelPatcher::KextInfo IntelBTPatcher_IOBluetoothInfo {
    "com.apple.iokit.IOBluetoothFamily",
    IntelBTPatcher_IOBluetoothFamily,
    1,
    {true, true},
    {},
    KernelPatcher::KextInfo::Unloaded
};

static const char *IntelBTPatcher_IOUSBHostFamily[] {
    "/System/Library/Extensions/IOUSBHostFamily.kext/Contents/MacOS/IOUSBHostFamily" };

static KernelPatcher::KextInfo IntelBTPatcher_IOUsbHostInfo {
    "com.apple.iokit.IOUSBHostFamily",
    IntelBTPatcher_IOUSBHostFamily,
    1,
    {true, true},
    {},
    KernelPatcher::KextInfo::Unloaded
};

bool CIntelBTPatcher::_randomAddressInit = false;
void * volatile CIntelBTPatcher::_eventPipe = nullptr;
void * volatile CIntelBTPatcher::_eventPipeDevice = nullptr;
bool CIntelBTPatcher::_dlePatchEnabled = false;
bool CIntelBTPatcher::_dleLookupEnabled = false;

static thread_call_t eventPipeLookupCall = nullptr;
static void eventPipeLookup(thread_call_param_t, thread_call_param_t device);

bool CIntelBTPatcher::init()
{
    DBGLOG(DRV_NAME, "%s", __PRETTY_FUNCTION__);
    callbackIBTPatcher = this;
    // Opt-in, in two steps so each can be tested on its own:
    //   -ibtdleio  only route IOUSBHostPipe::io (every read passes straight through)
    //   -ibtdle    route it and find the Intel event pipe, so the LE features reply gets patched
    // Without either, this kext routes exactly what upstream 2.5.0 routes.
    _dleLookupEnabled = checkKernelArgument("-ibtdle");
    _dlePatchEnabled = _dleLookupEnabled || checkKernelArgument("-ibtdleio");
    if (_dleLookupEnabled)
        eventPipeLookupCall = thread_call_allocate(eventPipeLookup, nullptr);
    SYSLOG(DRV_NAME, "LE Data Length Extension hiding: io %d lookup %d", _dlePatchEnabled, _dleLookupEnabled);
    if (getKernelVersion() < KernelVersion::Monterey) {
        lilu.onKextLoadForce(&IntelBTPatcher_IOBluetoothInfo, 1,
        [](void *user, KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
            callbackIBTPatcher->processKext(patcher, index, address, size);
        }, this);
    } else {
        lilu.onKextLoadForce(&IntelBTPatcher_IOUsbHostInfo, 1,
        [](void *user, KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
            callbackIBTPatcher->processKext(patcher, index, address, size);
        }, this);
    }
    return true;
}

void CIntelBTPatcher::free()
{
    DBGLOG(DRV_NAME, "%s", __PRETTY_FUNCTION__);
}

void CIntelBTPatcher::processKext(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size)
{
    DBGLOG(DRV_NAME, "%s", __PRETTY_FUNCTION__);
    if (getKernelVersion() < KernelVersion::Monterey) {
        if (IntelBTPatcher_IOBluetoothInfo.loadIndex == index) {
            DBGLOG(DRV_NAME, "%s", IntelBTPatcher_IOBluetoothInfo.id);
            
            KernelPatcher::RouteRequest findQueueRequestRequest {
                "__ZN25IOBluetoothHostController17FindQueuedRequestEtP22BluetoothDeviceAddresstbPP21IOBluetoothHCIRequest",
                newFindQueueRequest,
                oldFindQueueRequest
            };
            patcher.routeMultiple(index, &findQueueRequestRequest, 1, address, size);
            if (patcher.getError() == KernelPatcher::Error::NoError) {
                DBGLOG(DRV_NAME, "routed %s", findQueueRequestRequest.symbol);
            } else {
                SYSLOG(DRV_NAME, "failed to resolve %s, error = %d", findQueueRequestRequest.symbol, patcher.getError());
                patcher.clearError();
            }
            
        }
    } else {
        if (IntelBTPatcher_IOUsbHostInfo.loadIndex == index) {
            SYSLOG(DRV_NAME, "%s", IntelBTPatcher_IOUsbHostInfo.id);
            
            KernelPatcher::RouteRequest hostDeviceRequest {
            "__ZN15IOUSBHostDevice13deviceRequestEP9IOServiceRN11StandardUSB13DeviceRequestEPvP18IOMemoryDescriptorRjP19IOUSBHostCompletionj",
                newHostDeviceRequest,
                oldHostDeviceRequest
            };
            patcher.routeMultiple(index, &hostDeviceRequest, 1, address, size);
            if (patcher.getError() == KernelPatcher::Error::NoError) {
                SYSLOG(DRV_NAME, "routed %s", hostDeviceRequest.symbol);
            } else {
                SYSLOG(DRV_NAME, "failed to resolve %s, error = %d", hostDeviceRequest.symbol, patcher.getError());
                patcher.clearError();
            }

            if (!_dlePatchEnabled)
                return;

            KernelPatcher::RouteRequest asyncIORequest {
                "__ZN13IOUSBHostPipe2ioEP18IOMemoryDescriptorjP19IOUSBHostCompletionj",
                newAsyncIO,
                oldAsyncIO
            };
            patcher.routeMultiple(index, &asyncIORequest, 1, address, size);
            if (patcher.getError() == KernelPatcher::Error::NoError) {
                SYSLOG(DRV_NAME, "routed %s", asyncIORequest.symbol);
            } else {
                SYSLOG(DRV_NAME, "failed to resolve %s, error = %d", asyncIORequest.symbol, patcher.getError());
                patcher.clearError();
            }
        }
    }
}

#pragma mark - For Bigsur-, patch unhandled 0x2019 opcode

#define HCI_OP_LE_START_ENCRYPTION 0x2019

IOReturn CIntelBTPatcher::newFindQueueRequest(void *that, unsigned short arg1, void *addr, unsigned short arg2, bool arg3, void **hciRequestPtr)
{
    IOReturn ret = FunctionCast(newFindQueueRequest, callbackIBTPatcher->oldFindQueueRequest)(that, arg1, addr, arg2, arg3, hciRequestPtr);
    if (ret != 0 && arg1 == HCI_OP_LE_START_ENCRYPTION) {
        ret = FunctionCast(newFindQueueRequest, callbackIBTPatcher->oldFindQueueRequest)(that, arg1, addr, 0xffff, arg3, hciRequestPtr);
        DBGLOG(DRV_NAME, "%s ret: %d arg1: 0x%04x arg2: 0x%04x arg3: %d ptr: %p", __FUNCTION__, ret, arg1, arg2, arg3, *hciRequestPtr);
    }
    return ret;
}

#pragma mark - For Monterey+ patch for intercept HCI REQ and RESP

StandardUSB::DeviceRequest randomAddressRequest;
// Hardcoded Random Address HCI Command
const uint8_t randomAddressHci[9] = {0x05, 0x20, 0x06, 0x94, 0x50, 0x64, 0xD0, 0x78, 0x6B}; 
IOBufferMemoryDescriptor *writeHCIDescriptor = nullptr;

#define MAX_HCI_BUF_LEN             255
#define HCI_OP_RESET                0x0c03
#define HCI_OP_LE_SET_SCAN_PARAM    0x200B
#define HCI_OP_LE_SET_SCAN_ENABLE   0x200C

#define VENDOR_USB_INTEL            0x8087
#define HCI_EVENT_PIPE_ADDRESS      0x81

// Lilu RELEASE builds drop SYSLOG, so progress is published as properties on the plugin's
// IOService instead: ioreg -l -w0 -c IntelBTPatcher | grep DLE
static void reportDLE(const char *key, uint32_t value)
{
    if (ADDPR(selfInstance))
        ADDPR(selfInstance)->setProperty(key, value, 32);
}

// Last non-Intel device seen sending class requests, so it is not queued for lookup again.
static void * volatile lastOtherDevice = nullptr;

// Look up the interrupt IN pipe of the Intel device that is sending HCI commands. Runs on a
// thread call, never inside deviceRequest, so it cannot touch a device that is still being
// enumerated or contend for USB gates held by the request path. Redone when the device object
// changes (re-enumeration after firmware load).
static void findEventPipe(IOUSBHostDevice *device)
{
    if (device == CIntelBTPatcher::_eventPipeDevice || device == lastOtherDevice)
        return;
    const StandardUSB::DeviceDescriptor *deviceDescriptor = device->getDeviceDescriptor();
    if (!deviceDescriptor || deviceDescriptor->idVendor != VENDOR_USB_INTEL) {
        lastOtherDevice = device;
        return;
    }
    CIntelBTPatcher::_eventPipeDevice = device;
    static uint32_t lookups = 0;
    reportDLE("DLEIoHooked", callbackIBTPatcher->oldAsyncIO ? 1 : 0);
    reportDLE("DLEPipeLookups", ++lookups);

    OSIterator *iterator = device->getChildIterator(gIOServicePlane);
    if (!iterator)
        return;
    while (OSObject *child = iterator->getNextObject()) {
        IOUSBHostInterface *interface = OSDynamicCast(IOUSBHostInterface, child);
        if (!interface)
            continue;
        IOUSBHostPipe *pipe = interface->copyPipe(HCI_EVENT_PIPE_ADDRESS);
        if (pipe) {
            // Only the address is kept, for comparison in newAsyncIO.
            CIntelBTPatcher::_eventPipe = pipe;
            pipe->release();
            static uint32_t found = 0;
            reportDLE("DLEPipeFound", ++found);
            SYSLOG(DRV_NAME, "[DLE] found HCI event pipe via device lookup");
            break;
        }
    }
    iterator->release();
}

static void eventPipeLookup(thread_call_param_t, thread_call_param_t device)
{
    findEventPipe((IOUSBHostDevice *)device);
    ((IOUSBHostDevice *)device)->release();
}

#define USB_REQUEST_TYPE_MASK       0x60
#define USB_REQUEST_TYPE_CLASS      0x20

// Called from deviceRequest: only queues work, and only for class requests (HCI commands go out
// as class requests; enumeration uses standard ones).
static void queueEventPipeLookup(IOUSBHostDevice *device, const StandardUSB::DeviceRequest &request)
{
    if ((request.bmRequestType & USB_REQUEST_TYPE_MASK) != USB_REQUEST_TYPE_CLASS)
        return;
    if (device == CIntelBTPatcher::_eventPipeDevice || device == lastOtherDevice || !eventPipeLookupCall)
        return;
    device->retain();
    // Returns true if a lookup was already pending; that one keeps its own reference.
    if (thread_call_enter1(eventPipeLookupCall, device))
        device->release();
}

IOReturn CIntelBTPatcher::newHostDeviceRequest(void *that, IOService *provider, StandardUSB::DeviceRequest &request, void *data, IOMemoryDescriptor *descriptor, unsigned int &length, IOUSBHostCompletion *completion, unsigned int timeout)
{
    HciCommandHdr *hdr = nullptr;
    uint32_t hdrLen = 0;
    char hciBuf[MAX_HCI_BUF_LEN] = {0};

    if (_dleLookupEnabled && callbackIBTPatcher->oldAsyncIO && that)
        queueEventPipeLookup((IOUSBHostDevice *)that, request);

    if (data == nullptr) {
        if (descriptor != nullptr &&
            (getKernelVersion() < KernelVersion::Sequoia || !descriptor->prepare(kIODirectionOut))) {
            if (descriptor->getLength() > 0) {
                descriptor->readBytes(0, hciBuf, min(descriptor->getLength(), MAX_HCI_BUF_LEN));
                hdrLen = (uint32_t)min(descriptor->getLength(), MAX_HCI_BUF_LEN);
            }
            if (getKernelVersion() >= KernelVersion::Sequoia)
                descriptor->complete(kIODirectionOut);
        }
        hdr = (HciCommandHdr *)hciBuf;
        if (hdr->opcode == HCI_OP_LE_SET_SCAN_PARAM) {
            if (!_randomAddressInit) {
                randomAddressRequest.bmRequestType = makeDeviceRequestbmRequestType(kRequestDirectionOut, kRequestTypeClass, kRequestRecipientInterface);
                randomAddressRequest.bRequest = 0xE0;
                randomAddressRequest.wIndex = 0;
                randomAddressRequest.wValue = 0;
                randomAddressRequest.wLength = 9;
                length = 9;
                if (writeHCIDescriptor == nullptr)
                    writeHCIDescriptor = IOBufferMemoryDescriptor::withBytes(randomAddressHci, 9, kIODirectionOut);
                writeHCIDescriptor->prepare(kIODirectionOut);
                IOReturn ret = FunctionCast(newHostDeviceRequest, callbackIBTPatcher->oldHostDeviceRequest)(that, provider, randomAddressRequest, nullptr, writeHCIDescriptor, length, nullptr, timeout);
                writeHCIDescriptor->complete();
                const char *randAddressDump = _hexDumpHCIData((uint8_t *)randomAddressHci, 9);
                if (randAddressDump) {
                    SYSLOG(DRV_NAME, "[PATCH] Sending Random Address HCI %lld %s", ret, randAddressDump);
                    IOFree((void *)randAddressDump, 9 * 3 + 1);
                }
                _randomAddressInit = true;
                SYSLOG(DRV_NAME, "[PATCH] Resend LE SCAN PARAM HCI %lld", ret);
            }
        }
    } else {
        hdr = (HciCommandHdr *)data;
        hdrLen = request.wLength - 3;
    }
    if (hdr) {
        // HCI reset, we need to send Random address again
        if (hdr->opcode == HCI_OP_RESET)
            _randomAddressInit = false;
#if DEBUG
        DBGLOG(DRV_NAME, "[%s] bRequest: 0x%x direction: %s type: %s recipient: %s wValue: 0x%02x wIndex: 0x%02x opcode: 0x%04x len: %d length: %d async: %d", provider->getName(), request.bRequest, requestDirectionNames[(request.bmRequestType & kDeviceRequestDirectionMask) >> kDeviceRequestDirectionPhase], requestRecipientNames[(request.bmRequestType & kDeviceRequestRecipientMask) >> kDeviceRequestRecipientPhase], requestTypeNames[(request.bmRequestType & kDeviceRequestTypeMask) >> kDeviceRequestTypePhase], request.wValue, request.wIndex, hdr->opcode, hdr->len, request.wLength, completion != nullptr);
        if (hdrLen) {
            const char *dump = _hexDumpHCIData((uint8_t *)hdr, hdrLen);
            if (dump) {
                DBGLOG(DRV_NAME, "[Request]: %s", dump);
                IOFree((void *)dump, hdrLen * 3 + 1);
            }
        }
#endif
    }
    return FunctionCast(newHostDeviceRequest, callbackIBTPatcher->oldHostDeviceRequest)(that, provider, request, data, descriptor, length, completion, timeout);
}

#pragma mark - Hide LE Data Length Extension from bluetoothd

// bluetoothd sends LE Set Data Length to every peer that supports DLE and then waits for an
// LE Data Length Change event before it calls the link ready. Intel firmware only sends that
// event when the lengths actually change, so a peer that keeps the default 27 bytes (e.g. some
// BLE mice) never becomes ready: no GATT, no pairing, disconnect after ~30 s. Clearing the
// DLE bit in the controller's LE feature mask makes bluetoothd skip that step for every link.

#define HCI_EV_CMD_COMPLETE             0x0E
#define HCI_OP_LE_READ_LOCAL_FEATURES   0x2003
#define LE_FEATURE_DATA_LENGTH_EXT      0x20    // LE feature bit 5, in the first feature byte

// Command Complete: evt, plen, ncmd, opcode lo, opcode hi, status, features[8]
#define LE_FEATURES_EVT_LEN             14
#define LE_FEATURES_BYTE0_OFFSET        6

static void hideDataLengthExtension(IOMemoryDescriptor *dataBuffer, uint32_t bytesTransferred)
{
    if (!dataBuffer || bytesTransferred < LE_FEATURES_EVT_LEN)
        return;
    if (dataBuffer->prepare() != kIOReturnSuccess)
        return;
    uint8_t evt[LE_FEATURES_BYTE0_OFFSET + 1];
    if (dataBuffer->readBytes(0, evt, sizeof(evt)) == sizeof(evt) &&
        evt[0] == HCI_EV_CMD_COMPLETE &&
        evt[1] >= LE_FEATURES_EVT_LEN - 2 &&
        evt[3] == (HCI_OP_LE_READ_LOCAL_FEATURES & 0xFF) &&
        evt[4] == (HCI_OP_LE_READ_LOCAL_FEATURES >> 8) &&
        evt[5] == 0x00 &&
        (evt[LE_FEATURES_BYTE0_OFFSET] & LE_FEATURE_DATA_LENGTH_EXT)) {
        uint8_t patched = evt[LE_FEATURES_BYTE0_OFFSET] & ~LE_FEATURE_DATA_LENGTH_EXT;
        dataBuffer->writeBytes(LE_FEATURES_BYTE0_OFFSET, &patched, 1);
        static uint32_t patchedCount = 0;
        reportDLE("DLEFeaturesPatched", ++patchedCount);
        SYSLOG(DRV_NAME, "[DLE] LE features byte0 0x%02x -> 0x%02x", evt[LE_FEATURES_BYTE0_OFFSET], patched);
    }
    dataBuffer->complete();
}

static void eventReadCompletion(void *owner, void *parameter, IOReturn status, uint32_t bytesTransferred)
{
    EventReadContext *context = (EventReadContext *)owner;
    IOUSBHostCompletion orig = context->orig;
    if (status == kIOReturnSuccess)
        hideDataLengthExtension(context->dataBuffer, bytesTransferred);
    IOFree(context, sizeof(EventReadContext));
    if (orig.action)
        orig.action(orig.owner, parameter, status, bytesTransferred);
}

IOReturn CIntelBTPatcher::newAsyncIO(void *that, IOMemoryDescriptor *dataBuffer, uint32_t dataBufferLength, IOUSBHostCompletion *completion, uint32_t completionTimeoutMs)
{
    if (that && that == _eventPipe && completion) {
        EventReadContext *context = (EventReadContext *)IOMalloc(sizeof(EventReadContext));
        if (context) {
            context->orig = *completion;
            context->dataBuffer = dataBuffer;
            context->hook.owner = context;
            context->hook.action = eventReadCompletion;
            context->hook.parameter = completion->parameter;
            // The completion is copied by io() and only called when io() succeeds.
            IOReturn ret = FunctionCast(newAsyncIO, callbackIBTPatcher->oldAsyncIO)(that, dataBuffer, dataBufferLength, &context->hook, completionTimeoutMs);
            if (ret != kIOReturnSuccess) {
                IOFree(context, sizeof(EventReadContext));
            } else {
                static bool reported = false;
                if (!reported) {
                    reported = true;
                    reportDLE("DLEReadsWrapped", 1);
                }
            }
            return ret;
        }
    }
    return FunctionCast(newAsyncIO, callbackIBTPatcher->oldAsyncIO)(that, dataBuffer, dataBufferLength, completion, completionTimeoutMs);
}
