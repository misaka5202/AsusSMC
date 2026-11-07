//
//  VirtualAppleKeyboard.cpp
//  VirtualAppleKeyboard
//
//  Copyright © 2018-2020 Le Bao Hiep. All rights reserved.
//

#include "VirtualAppleKeyboard.hpp"

#define super IOHIDDevice
OSDefineMetaClassAndStructors(VirtualAppleKeyboard, super);

// Where keyboard backlight writes go. Set by the owning driver; null until
// then, in which case writes are accepted and recorded but not forwarded.
VAKBacklightSink VirtualAppleKeyboard::backlightSink = nullptr;
void *VirtualAppleKeyboard::backlightContext = nullptr;

const uint8_t virt_reportDescriptor[] = {
    0x05, 0x01,       // Usage Page (Generic Desktop)
    0x09, 0x06,       // Usage (Keyboard)
    0xa1, 0x01,       // Collection (Application)
    0x85, 0x01,       //   Report Id (1)
    0x05, 0x07,       //   Usage Page (Keyboard/Keypad)
    0x19, 0xe0,       //   Usage Minimum........... (224)
    0x29, 0xe7,       //   Usage Maximum........... (231)
    0x15, 0x00,       //   Logical Minimum......... (0)
    0x25, 0x01,       //   Logical Maximum......... (1)
    0x75, 0x01,       //   Report Size............. (1)
    0x95, 0x08,       //   Report Count............ (8)
    0x81, 0x02,       //   Input...................(Data, Variable, Absolute)
                      //
    0x95, 0x01,       //   Report Count............ (1)
    0x75, 0x08,       //   Report Size............. (8)
    0x81, 0x01,       //   Input...................(Constant)
                      //
    0x95, 0x20,       //   Report Count............ (32)
    0x75, 0x08,       //   Report Size............. (8)
    0x15, 0x00,       //   Logical Minimum......... (0)
    0x26, 0xff, 0x00, //   Logical Maximum......... (255)
    0x05, 0x07,       //   Usage Page (Keyboard/Keypad)
    0x19, 0x00,       //   Usage Minimum........... (0)
    0x29, 0xff,       //   Usage Maximum........... (255)
    0x81, 0x00,       //   Input...................(Data, Array, Absolute)
    0xc0,             // End Collection

    0x05, 0x0c,       // Usage Page (Consumer)
    0x09, 0x01,       // Usage 1 (kHIDUsage_Csmr_ConsumerControl)
    0xa1, 0x01,       // Collection (Application)
    0x85, 0x02,       //   Report Id (2)
    0x05, 0x0c,       //   Usage Page (Consumer)
    0x95, 0x20,       //   Report Count............ (32)
    0x75, 0x08,       //   Report Size............. (8)
    0x15, 0x00,       //   Logical Minimum......... (0)
    0x26, 0xff, 0x00, //   Logical Maximum......... (255)
    0x19, 0x00,       //   Usage Minimum........... (0)
    0x29, 0xff,       //   Usage Maximum........... (255)
    0x81, 0x00,       //   Input...................(Data, Array, Absolute)
    0xc0,             // End Collection

    0x06, 0x00, 0xff, // Usage Page (kHIDPage_AppleVendor)
    0x09, 0x01,       // Usage 1 (kHIDUsage_AppleVendor_TopCase)
    0xa1, 0x01,       // Collection (Application)
    0x85, 0x03,       //   Report Id (3)
    0x05, 0xff,       //   Usage Page (kHIDPage_AppleVendorTopCase)
    0x95, 0x20,       //   Report Count............ (32)
    0x75, 0x08,       //   Report Size............. (8)
    0x15, 0x00,       //   Logical Minimum......... (0)
    0x26, 0xff, 0x00, //   Logical Maximum......... (255)
    0x19, 0x00,       //   Usage Minimum........... (0)
    0x29, 0xff,       //   Usage Maximum........... (255)
    0x81, 0x00,       //   Input...................(Data, Array, Absolute)
    0xc0,             // End Collection

    0x06, 0x00, 0xff, // Usage Page (kHIDPage_AppleVendor)
    0x09, 0x0f,       // Usage 15 (kHIDUsage_AppleVendor_KeyboardBacklight)
    0xa1, 0x01,       // Collection (Application)
    0x85, 0xbf,       //   Report ID (191)
    0x06, 0x00, 0xff, //   Usage Page (kHIDUsage_AppleVendor_KeyboardBacklight)
    0x95, 0x20,       //   Report Count............ (32)
    0x75, 0x08,       //   Report Size............. (8)
    0x15, 0x00,       //   Logical Minimum......... (0)
    0x26, 0xff, 0x00, //   Logical Maximum......... (255)
    0x19, 0x00,       //   Usage Minimum........... (0)
    0x29, 0xff,       //   Usage Maximum........... (255)
    0x81, 0x00,       //   Input...................(Data, Array, Absolute)
    0x91, 0x00,       //   Output..................(Data, Array, Absolute)
    0xb1, 0x00,       //   Feature.................(Data, Array, Absolute)
    0xc0,             // End Collection
};

// Where VirtualAppleKeyboard publishes the current level back to macOS.
// Where VirtualAppleKeyboard publishes the current level back to macOS.
// Byte 4 of the get status blob; scale is 0..0x20 as in the shipped blob.
uint8_t VirtualAppleKeyboard::reportedLevel = 0;

// Request counters, published so the question "is macOS reading this at all"
// is answered by measurement.
uint32_t VirtualAppleKeyboard::getReportCount = 0;
uint32_t VirtualAppleKeyboard::setReportCount = 0;

// Append one (id, type, length) triple to a request log and publish it.
//
// This must not allocate: the r30 version used OSArray::withCapacity(), which
// returns a temporary smart pointer under the current MacKernelSDK - it
// released the array when the statement ended, and the first getReport after
// boot dereferenced a dangling pointer. That is the boot panic. The log is
// therefore a fixed buffer in a byte-property, which IORegistryEntry copies.
static void recordRequest(IOHIDDevice *self, uint8_t id, uint32_t type,
                          uint32_t len, const char *key, uint8_t *storage,
                          uint32_t *count) {
    uint8_t *entries = storage;         // triples live here
    uint8_t *idx = storage + 24;        // count lives in the last byte
    if (!entries || !idx || !count) {
        return;
    }
    if (*idx >= 8) {
        return;
    }
    entries[(*idx) * 3 + 0] = id;
    entries[(*idx) * 3 + 1] = (uint8_t)type;
    entries[(*idx) * 3 + 2] = (uint8_t)len;
    (*idx)++;
    *count = *idx;
    self->setProperty(key, storage, 25);
}


bool VirtualAppleKeyboard::handleStart(IOService *provider) {
    if (!super::handleStart(provider)) {
        return false;
    }

    setProperty("AppleVendorSupported", kOSBooleanTrue);
    setProperty("Built-In", kOSBooleanTrue);
    setProperty("HIDDefaultBehavior", kOSBooleanTrue);

    return true;
}

IOReturn VirtualAppleKeyboard::newReportDescriptor(IOMemoryDescriptor **descriptor) const {
    *descriptor = IOBufferMemoryDescriptor::withBytes(virt_reportDescriptor, sizeof(virt_reportDescriptor), kIODirectionNone);
    return kIOReturnSuccess;
}

IOReturn VirtualAppleKeyboard::getReport(IOMemoryDescriptor *report, IOHIDReportType reportType, IOOptionBits options) {
    uint8_t report_id = options & 0xff;

    // Record every request. This is the question that decides everything about
    // the System Settings brightness control: does macOS ever read this
    // collection, with which report type, and how many bytes does it expect?
    // Answered by numbers rather than by guessing, because the previous rounds
    // of guessing cost far more than asking did.
    getReportCount++;
    setProperty("AsusSMCHIDGetCount", (uint64_t)getReportCount, 32);
    setProperty("AsusSMCHIDGetId", (uint64_t)report_id, 32);
    setProperty("AsusSMCHIDGetType", (uint64_t)reportType, 32);
    setProperty("AsusSMCHIDGetLen", (uint64_t)(report ? report->getLength() : 0), 32);
    recordRequest(this, report_id, (uint32_t)reportType,
                  report ? (uint32_t)report->getLength() : 0,
                  "AsusSMCHIDGetLog", getLogBytes, &getLogCount);

    if (report_id == 0xbf && report != nullptr) {
        // Backlight status. Byte 4 reads as the level on the 0x20 scale that
        // the shipped blob uses, byte 5 as the maximum. The whole requested
        // length is filled: returning eight bytes for a Feature report that
        // asks for more is exactly the kind of short answer a caller may
        // reject, and it costs nothing to avoid.
        uint8_t buffer[40];
        memset(buffer, 0, sizeof(buffer));

        const uint8_t blob[] = {0x00, 0x01, 0x00, 0x00, reportedLevel, 0x20, 0x02, 0x00};
        memcpy(buffer, blob, sizeof(blob));

        IOByteCount want = report->getLength();
        if (want > sizeof(buffer)) {
            want = sizeof(buffer);
        }
        if (want >= sizeof(blob)) {
            report->writeBytes(0, buffer, want);
        } else {
            report->writeBytes(0, blob, sizeof(blob));
        }
        return kIOReturnSuccess;
    }

    return kIOReturnSuccess;
}

IOReturn VirtualAppleKeyboard::setReport(IOMemoryDescriptor *report, IOHIDReportType reportType, IOOptionBits options) {
    uint8_t report_id = options & 0xff;

    setReportCount++;
    setProperty("AsusSMCHIDSetCount", (uint64_t)setReportCount, 32);
    setProperty("AsusSMCHIDSetId", (uint64_t)report_id, 32);
    setProperty("AsusSMCHIDSetType", (uint64_t)reportType, 32);
    recordRequest(this, report_id, (uint32_t)reportType,
                  report ? (uint32_t)report->getLength() : 0,
                  "AsusSMCHIDSetLog", setLogBytes, &setLogCount);

    if (report_id == 0xbf && report != nullptr) {
        uint8_t buf[40] = {};
        IOByteCount length = report->getLength();
        if (length > sizeof(buf)) {
            length = sizeof(buf);
        }
        if (length > 0 && report->readBytes(0, buf, length) == length) {
            // Record exactly what macOS sent. The payload layout of this
            // collection is not documented anywhere, so the first captures
            // calibrate the mapping rather than guessing it. Built by hand to
            // avoid pulling printf into this file.
            static const char hexDigits[] = "0123456789ABCDEF";
            char hex[3 * sizeof(buf)];
            size_t pos = 0;
            for (IOByteCount i = 0; i < length && pos + 3 < sizeof(hex); i++) {
                hex[pos++] = hexDigits[(buf[i] >> 4) & 0x0f];
                hex[pos++] = hexDigits[buf[i] & 0x0f];
                hex[pos++] = ' ';
            }
            hex[pos] = '\0';
            setProperty("AsusSMCHIDBacklightSet", hex);

            if (backlightSink) {
                backlightSink(buf, (uint32_t)length, backlightContext);
            }
        }
    }

    // Returning success here is the actual fix: the default IOHIDDevice
    // implementation returns kIOReturnUnsupported, and a rejected write is
    // what left the brightness control permanently disabled.
    return kIOReturnSuccess;
}

OSString *VirtualAppleKeyboard::newManufacturerString() const {
    return OSString::withCString("Apple Inc.");
}

OSString *VirtualAppleKeyboard::newProductString() const {
    return OSString::withCString("Virtual Apple Keyboard");
}

OSNumber *VirtualAppleKeyboard::newVendorIDNumber() const {
    return OSNumber::withNumber(0x5ac, 32);
}

OSNumber *VirtualAppleKeyboard::newProductIDNumber() const {
    return OSNumber::withNumber(0x276, 32);
}

OSNumber *VirtualAppleKeyboard::newLocationIDNumber() const {
    return OSNumber::withNumber(0x1000000, 32);
}

OSNumber *VirtualAppleKeyboard::newCountryCodeNumber() const {
    return OSNumber::withNumber(0x21, 32);
}

OSNumber *VirtualAppleKeyboard::newVersionNumber() const {
    return OSNumber::withNumber(0x895, 32);
}

OSNumber *VirtualAppleKeyboard::newPrimaryUsagePageNumber() const {
    return OSNumber::withNumber(kHIDPage_GenericDesktop, 32);
}

OSNumber *VirtualAppleKeyboard::newPrimaryUsageNumber() const {
    return OSNumber::withNumber(kHIDUsage_GD_Keyboard, 32);
}
