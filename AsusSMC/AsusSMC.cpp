//
//  AsusSMC.cpp
//  AsusSMC
//
//  Copyright © 2018-2020 Le Bao Hiep. All rights reserved.
//

#include "AsusSMC.hpp"

#include <IOKit/hid/IOHIDDevice.h>
#include <IOKit/IOMemoryDescriptor.h>

bool ADDPR(debugEnabled) = false;
uint32_t ADDPR(debugPrintDelay) = 0;

#define super IOService
OSDefineMetaClassAndStructors(AsusSMC, IOService)

bool AsusSMC::init(OSDictionary *dict) {
    if (!super::init(dict)) {
        return false;
    }

    _notificationServices = OSSet::withCapacity(1);

    kev.setVendorID("com.hieplpvip");
    kev.setEventCode(AsusSMCEventCode);

    atomic_init(&currentLux, 0);
    atomic_init(&currentFanSpeed, 0);

    return true;
}

IOService *AsusSMC::probe(IOService *provider, SInt32 *score) {
    if (!super::probe(provider, score)) {
        return NULL;
    }

    IOACPIPlatformDevice *dev = OSDynamicCast(IOACPIPlatformDevice, provider);
    if (!dev) {
        return NULL;
    }

    OSObject *obj;
    dev->evaluateObject("_UID", &obj);

    OSString *name = OSDynamicCast(OSString, obj);
    if (!name) {
        return NULL;
    }

    IOService *ret = NULL;
    if (name->isEqualTo("ATK")) {
        ret = this;
    }
    name->release();

    return ret;
}

bool AsusSMC::start(IOService *provider) {
    if (!provider || !super::start(provider)) {
        SYSLOG("atk", "failed to start parent");
        return false;
    }

    atkDevice = (IOACPIPlatformDevice *)provider;

    parse_WDG();

    initATKDevice();

    initALSDevice();

    initEC0Device();

    initBattery();

    // Version badge. Every diagnostic below is worthless without knowing which
    // build produced it: a boot that silently kept an older kext read exactly
    // like a failed experiment twice already. Bump kAsusSMCBuildTag whenever
    // behaviour changes and check it with
    //   ioreg -rn AsusSMC -l | grep AsusSMCBuild
    setProperty("AsusSMCBuild", kAsusSMCBuildTag);

    // VirtualAppleKeyboard publishes an Apple Vendor KeyboardBacklight
    // collection (usage page 0xFF00, usage 0x0F, report id 191), which is what
    // makes System Settings show the keyboard brightness panel at all. -asussmcnokbd
    // leaves it out, so the panel source can be confirmed in one boot.
    if (!checkKernelArgument("-asussmcnokbd")) {
        initVirtualKeyboard();
    } else {
        setProperty("AsusSMCVirtualKeyboard", "disabled by boot-arg");
    }

    startATKDevice();

    workloop = getWorkLoop();
    if (!workloop) {
        DBGLOG("atk", "Failed to get workloop");
        return false;
    }
    workloop->retain();

    command_gate = IOCommandGate::commandGate(this);
    if (!command_gate || (workloop->addEventSource(command_gate) != kIOReturnSuccess)) {
        DBGLOG("atk", "Could not open command gate");
        return false;
    }

    setProperty("IsTouchpadEnabled", true);
    setProperty("Copyright", "Copyright © 2018-2020 Le Bao Hiep. All rights reserved.");

    extern kmod_info_t kmod_info;
    setProperty("AsusSMC-Version", kmod_info.version);
#ifdef DEBUG
    setProperty("AsusSMC-Build", "Debug");
#else
    setProperty("AsusSMC-Build", "Release");
#endif

    registerNotifications();

    registerVSMC();

    // System power events. Ported from upstream PR #62, merged into 1.2.2 and
    // confirmed by several users: without joining the power tree the keyboard
    // stays dark after a wake, because nothing restores the level.
    subscribePowerEvents(provider);

    registerService();

    return true;
}

/**
 *  System power events, ported from upstream PR #62 (merged into 1.2.2).
 *
 *  Two states are enough: off and on. When the system goes to sleep the
 *  keyboard is blanked, and on wake the remembered level is restored. Without
 *  this the keyboard either stays lit during sleep or comes back dark after
 *  waking, because nothing else restores it.
 */
// kIOPMPowerOff does not exist in IOKit; upstream's patch defines its own zero
// for the off state, and so does this one. kIOPMPowerOn does exist.
#define kPowerStateOff 0
#define kPowerStateCount 2

static IOPMPowerState powerStates[kPowerStateCount] = {
    {1, kPowerStateOff, kPowerStateOff, kPowerStateOff, 0, 0, 0, 0, 0, 0, 0, 0},
    {1, kIOPMPowerOn, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0},
};

IOReturn AsusSMC::setPowerState(unsigned long powerStateOrdinal, IOService *whatDevice) {
    if (powerStateOrdinal == kPowerStateOff) {
        // Sleep, shutdown or reboot. Blanking here is what keeps the keyboard
        // from glowing through the night.
        int ret = sendKbdBacklightViaHID(0);
        SYSLOG("kbd", "system sleep -> keyboard backlight off (ret=0x%X)", (unsigned)ret);
    } else {
        int ret = sendKbdBacklightViaHID(kbdLevel);
        SYSLOG("kbd", "system wake -> keyboard backlight %u (ret=0x%X)",
               (unsigned)kbdLevel, (unsigned)ret);
    }
    return kIOPMAckImplied;
}

void AsusSMC::subscribePowerEvents(IOService *provider) {
    PMinit();
    provider->joinPMtree(this);
    IOReturn ret = registerPowerDriver(this, powerStates, kPowerStateCount);
    pmJoined = (ret == kIOReturnSuccess);
    setProperty("AsusSMCPowerEvents", pmJoined ? "registered" : "failed");
    SYSLOG("kbd", "power events: registerPowerDriver ret=0x%X", (unsigned)ret);
}

void AsusSMC::stop(IOService *provider) {
    if (pmJoined) {
        PMstop();
        pmJoined = false;
    }
    if (poller) {
        poller->cancelTimeout();
    }
    if (workloop && poller) {
        workloop->removeEventSource(poller);
    }
    if (workloop && command_gate) {
        workloop->removeEventSource(command_gate);
    }
    OSSafeReleaseNULL(workloop);
    OSSafeReleaseNULL(poller);
    OSSafeReleaseNULL(command_gate);

    _publishNotify->remove();
    _terminateNotify->remove();
    _notificationServices->flushCollection();
    OSSafeReleaseNULL(_publishNotify);
    OSSafeReleaseNULL(_terminateNotify);
    OSSafeReleaseNULL(_notificationServices);

    OSSafeReleaseNULL(kbdDevice);

    super::stop(provider);
    return;
}

IOReturn AsusSMC::message(uint32_t type, IOService *provider, void *argument) {
    DBGLOG("atk", "Received message: %u Type %x Provider %s", *((uint32_t *)argument), type, provider ? provider->getName() : "unknown");

    switch (type) {
        case kIOACPIMessageDeviceNotification:
        {
            if (directACPImessaging) {
                handleMessage(*((uint32_t *)argument));
            } else {
                uint32_t event = *((uint32_t *)argument);
                OSNumber *arg = OSNumber::withNumber(event, 32);
                uint32_t res;
                atkDevice->evaluateInteger("_WED", &res, (OSObject **)&arg, 1);
                arg->release();
                handleMessage(res);
            }
            break;
        }

        case kSetKeyboardBacklightMessage:
        {
            if (hasKeyboardBacklight) {
                // The caller (SMCKBrdBLightValue::update) has already mapped
                // the macOS 0..0xFFB range onto 0..ASUS_KBD_BACKLIGHT_MAX.
                uint8_t level = (uint8_t)(*((uint16_t *)argument));
                if (level > ASUS_KBD_BACKLIGHT_MAX) {
                    level = ASUS_KBD_BACKLIGHT_MAX;
                }

                // A level of 0 arriving here is ignored.
                //
                // Verified on this machine: macOS never drives LKSB, but
                // VirtualSMC calls update() with the key's default initialiser
                // at registration, and the default {val1 = 0, val2 = 1} maps
                // to level 0. Acting on it turned the keyboard off at boot.
                //
                // The display-following path writes 0 directly when the panel
                // sleeps, and asus-kbd can still turn the light off, so
                // nothing is lost by not honouring a stray zero here.
                if (level == 0) {
                    setProperty("AsusSMCLastBacklight", "level=0 ignored (SMC path)");
                    SYSLOG("kbd", "ignoring backlight level 0 from the SMC path");
                    break;
                }

                // Remember it: followDisplayPower() restores this after the
                // panel comes back.
                kbdLevel = level;

                // A request from the system while the panel is dark means the
                // two are no longer in sync; let the next poll decide.
                kbdDimmedForDisplay = false;

                if (keyboardBacklightViaWMI) {
                    // Preferred route: USB HID feature report. This is what
                    // Linux (hid-asus) and Windows both use, and it is the
                    // only path that actually reaches the EC on this board.
                    int hidRet = sendKbdBacklightViaHID(level);

                    char rec[160];
                    if (hidRet == kIOReturnSuccess) {
                        snprintf(rec, sizeof(rec),
                                 "level=%u path=HID ok", level);
                        setProperty("AsusSMCLastBacklight", rec);
                        SYSLOG("kbd", "keyboard backlight %s", rec);
                        break;
                    }

                    // Fall back to ACPI. Matches Linux asus-wmi's
                    // kbd_led_update(): ctrl_param = 0x80 | (level & 0x7F).
                    // The 0x80 bit is what EC0.SLKB tests; it is expected to be
                    // a no-op on this DSDT, but keep it for other boards.
                    uint32_t ctrl = ASUS_KBD_BACKLIGHT_ON_BIT | (level & 0x7F);
                    int ret = wmi_evaluate_method(ASUS_WMI_METHODID_DEVS,
                                                  ASUS_WMI_DEVID_KEYBOARD_BACKLIGHT,
                                                  ctrl);

                    snprintf(rec, sizeof(rec),
                             "level=%u path=ACPI hidRet=0x%X ctrl=0x%X devsRet=%d",
                             level, (unsigned)hidRet, ctrl, ret);
                    setProperty("AsusSMCLastBacklight", rec);
                    SYSLOG("kbd", "keyboard backlight %s", rec);
                } else {
                    OSNumber *arg = OSNumber::withNumber(level, 16);
                    IOReturn kr = atkDevice->evaluateObject("SKBV", NULL, (OSObject **)&arg, 1);
                    arg->release();

                    char rec[96];
                    snprintf(rec, sizeof(rec), "level=%u skbvRet=0x%X",
                             level, (unsigned)kr);
                    setProperty("AsusSMCLastBacklight", rec);
                    SYSLOG("kbd", "keyboard backlight %s", rec);
                }
            }
            break;
        }

        default:
            return kIOReturnInvalid;
    }
    return kIOReturnSuccess;
}

int AsusSMC::wmi_parse_guid(const char *in, char *out) {
    for (int i = 3; i >= 0; i--) {
        out += snprintf(out, 3, "%02X", in[i] & 0xFF);
    }

    out += snprintf(out, 2, "-");
    out += snprintf(out, 3, "%02X", in[5] & 0xFF);
    out += snprintf(out, 3, "%02X", in[4] & 0xFF);
    out += snprintf(out, 2, "-");
    out += snprintf(out, 3, "%02X", in[7] & 0xFF);
    out += snprintf(out, 3, "%02X", in[6] & 0xFF);
    out += snprintf(out, 2, "-");
    out += snprintf(out, 3, "%02X", in[8] & 0xFF);
    out += snprintf(out, 3, "%02X", in[9] & 0xFF);
    out += snprintf(out, 2, "-");

    for (int i = 10; i <= 15; i++) {
        out += snprintf(out, 3, "%02X", in[i] & 0xFF);
    }

    *out = '\0';
    return 0;
}

int AsusSMC::wmi_evaluate_method(uint32_t method_id, uint32_t arg0, uint32_t arg1) {
    OSObject *params[3];

    struct wmi_args args = {
        .arg0 = arg0,
        .arg1 = arg1
    };

    params[0] = OSNumber::withNumber(static_cast<uint32_t>(0), 32);
    params[1] = OSNumber::withNumber(method_id, 32);
    params[2] = OSData::withBytes(&args, sizeof(wmi_args));

    uint32_t val;
    IOReturn ret = atkDevice->evaluateInteger(wmi_method, &val, params, 3);
    params[0]->release();
    params[1]->release();
    params[2]->release();

    if (ret != kIOReturnSuccess) {
        DBGLOG("wmi", "wmi_evaluate_method failed");
        return -1;
    }

    if (val == 0xfffffffe) {
        DBGLOG("wmi", "wmi_evaluate_method invalid method_id");
        return -1;
    }

    return val;
}

int AsusSMC::wmi_get_devstate(uint32_t dev_id) {
    return wmi_evaluate_method(ASUS_WMI_METHODID_DSTS, dev_id, 0);
}

bool AsusSMC::wmi_dev_is_present(uint32_t dev_id) {
    int status = wmi_evaluate_method(ASUS_WMI_METHODID_DSTS, dev_id, 0);
    return status != -1 && (status & ASUS_WMI_DSTS_PRESENCE_BIT);
}

/**
 *  Detect the "generic WMI backlight" shape of the keyboard backlight.
 *
 *  Boards like the GA503QM have no SKBV method; their DSDT routes the
 *  backlight through DEVS device 0x00050021 -> EC0.SLKB. That device has
 *  no DSTS branch at all, so wmi_dev_is_present() cannot see it and asking
 *  DSTS just returns 0xFFFFFFFE.
 *
 *  Presence therefore has to be established structurally: the write has to
 *  go through ATKD.WMNB's DEVS chain, which only offers 0x00050021 when the
 *  board implements it. We confirm by writing "off" once; on a board without
 *  the device DEVS reports unsupported and nothing happens. Off is the safe
 *  value to probe with.
 */
bool AsusSMC::pci0Ec0HasKeyboardBacklight() {    if (wmi_dev_is_present(ASUS_WMI_DEVID_KEYBOARD_BACKLIGHT)) {
        return true;
    }

    int ret = wmi_evaluate_method(ASUS_WMI_METHODID_DEVS,
                                  ASUS_WMI_DEVID_KEYBOARD_BACKLIGHT,
                                  ASUS_KBD_BACKLIGHT_ON_BIT | 0);
    if (ret == -1 || ret == (int)ASUS_WMI_UNSUPPORTED_METHOD) {
        return false;
    }

    // DEVS on this chain answers 1 on success.
    return ret == 1;
}

/**
 *  Locate the ASUS keyboard's HID device.
 *
 *  The backlight cannot be reached over ACPI on this board (EC0.SLKB is a
 *  no-op), so the level has to go out as a USB HID feature report. Feature
 *  reports travel over the control endpoint, which is device wide, so any of
 *  the keyboard's HID interfaces will do - matching on the vendor id alone is
 *  enough and avoids depending on which collection happens to carry 0xFF31.
 *
 *  Resolution is lazy and retried: this driver matches an ACPI device, which
 *  is up long before the USB keyboard enumerates.
 */
bool AsusSMC::ensureKbdHIDDevice() {
    if (kbdHIDDevice) {
        return true;
    }

    OSDictionary *matching = IOService::serviceMatching("IOHIDDevice");
    if (!matching) {
        return false;
    }

    OSNumber *vid = OSNumber::withNumber((unsigned long long)ASUS_USB_VENDOR_ID, 32);
    if (vid) {
        matching->setObject("VendorID", vid);
        vid->release();
    }

    // Short timeout: this can be called from the SMC write path.
    IOService *svc = IOService::waitForMatchingService(matching, 100000000ULL /* 100 ms */);
    matching->release();

    if (!svc) {
        return false;
    }

    IOHIDDevice *hid = OSDynamicCast(IOHIDDevice, svc);
    if (hid) {
        hid->retain();
        kbdHIDDevice = hid;
        SYSLOG("kbd", "keyboard HID device found: %s", svc->getName());
    }
    svc->release();

    return kbdHIDDevice != nullptr;
}

/**
 *  Send the backlight feature report.
 *
 *  Layout matches drivers/hid/hid-asus.c asus_kbd_set_brightness():
 *    5A BA C5 C4 <level>   padded to 16 bytes
 *  and ROG-HID, which sends the same buffer through DriverKit.
 *
 *  @return kIOReturnSuccess when the report left the machine.
 */
int AsusSMC::sendKbdBacklightViaHID(uint8_t level) {
    if (!ensureKbdHIDDevice()) {
        return kIOReturnNotReady;
    }

    // The keyboard's report descriptor for collection 0xFF31/0x76 declares,
    // for report id 0x5A, a 5-byte input report and a 63-byte feature report
    // (MaxFeatureReportSize reports 64, counting the id byte). The payload is
    // therefore padded to 64 bytes, which is also the length that is proven to
    // work: the userspace tool drove the backlight with exactly this layout.
    uint8_t buf[64];
    memset(buf, 0, sizeof(buf));
    buf[0] = 0x5A;   // report id
    buf[1] = 0xBA;   // command
    buf[2] = 0xC5;   // illumination down
    buf[3] = 0xC4;   // illumination up
    buf[4] = level;

    IOBufferMemoryDescriptor *report =
        IOBufferMemoryDescriptor::withBytes(buf, sizeof(buf), kIODirectionOut);
    if (!report) {
        return kIOReturnNoMemory;
    }

    // The report id also goes in the low 8 bits of options; this is how
    // IOHIDDevice::setReport is documented.
    IOReturn kr = kbdHIDDevice->setReport(report, kIOHIDReportTypeFeature, 0x5A);
    report->release();

    return (int)kr;
}

void AsusSMC::recordKbdWrite(uint8_t val1, uint8_t val2, uint16_t packed, uint16_t level) {
    kbdWriteCount++;
    char rec[128];
    snprintf(rec, sizeof(rec),
             "val1=%u val2=%u packed=0x%X level=%u count=%u",
             val1, val2, packed, level, (unsigned)kbdWriteCount);
    setProperty("AsusSMCLKSB", rec);
}

void AsusSMC::recordLKSAccess(uint32_t key, bool isWrite, uint16_t value) {
    if (key > 1) {
        return;
    }
    if (isWrite) {
        lksWrites[key]++;
        lksLastValue = value;
    } else {
        lksReads[key]++;
    }
    publishLKSAccess();
}

void AsusSMC::publishLKSAccess() {
    // Published even when every counter is zero, which is exactly the case
    // that needs diagnosing: a keyboard brightness control that never touches
    // these keys.
    char rec[160];
    snprintf(rec, sizeof(rec),
             "lksbR=%u lksbW=%u lkssR=%u lkssW=%u lastW=0x%X",
             (unsigned)lksReads[0], (unsigned)lksWrites[0],
             (unsigned)lksReads[1], (unsigned)lksWrites[1],
             (unsigned)lksLastValue);
    setProperty("AsusSMCLKSAccess", rec);
}

void AsusSMC::parse_WDG() {
    OSObject *wdg;
    if (atkDevice->evaluateObject("_WDG", &wdg) != kIOReturnSuccess) {
        SYSLOG("wmi", "No method _WDG!");
        return;
    }

    OSData *data = OSDynamicCast(OSData, wdg);
    if (!data) {
        SYSLOG("guid", "Cast WDG error!");
        return;
    }

    int total = data->getLength() / sizeof(struct guid_block);

    char guid_string[37];

    for (int i = 0; i < total; i++) {
        struct guid_block *g = (struct guid_block *) data->getBytesNoCopy(i * sizeof(struct guid_block), sizeof(struct guid_block));
        wmi_parse_guid(g->guid, guid_string);

        if (strncmp(guid_string, ASUS_WMI_MGMT_GUID, 36) == 0) {
            snprintf(wmi_method, 5, "WM%c%c", g->object_id[0], g->object_id[1]);
            DBGLOG("wmi", "parse_WDG found WMI method %s", wmi_method);
            return;
        }
    }

    // Couldn't find WMI method. Let's assume it's WMNB
    SYSLOG("wmi", "parse_WDG couldn't find WMI method");
    lilu_os_strncpy(wmi_method, "WMNB", 5);
}

void AsusSMC::initATKDevice() {
    wmi_evaluate_method(ASUS_WMI_METHODID_INIT, 0, 0);
}

void AsusSMC::initALSDevice() {
    auto dict = IOService::nameMatching("AppleACPIPlatformExpert");
    if (!dict) {
        SYSLOG("als", "WTF? Failed to create matching dictionary");
        return;
    }

    auto acpi = IOService::waitForMatchingService(dict);
    dict->release();

    if (!acpi) {
        SYSLOG("als", "WTF? No ACPI");
        return;
    }

    acpi->release();

    dict = IOService::nameMatching("ACPI0008");
    if (!dict) {
        SYSLOG("als", "WTF? Failed to create matching dictionary");
        return;
    }

    auto deviceIterator = IOService::getMatchingServices(dict);
    dict->release();

    if (!deviceIterator) {
        SYSLOG("als", "No iterator");
        return;
    }

    alsDevice = OSDynamicCast(IOACPIPlatformDevice, deviceIterator->getNextObject());
    deviceIterator->release();

    if (!alsDevice) {
        SYSLOG("als", "ACPI0008 device not found");
        return;
    }

    if (alsDevice->validateObject("_ALI") != kIOReturnSuccess || !refreshALS(false)) {
        SYSLOG("als", "No functional method _ALI on ALS device");
        return;
    }

    SYSLOG("als", "Found ALS Device %s", alsDevice->getName());
}

void AsusSMC::initEC0Device() {
    isTACHAvailable = true;

    auto dict = IOService::nameMatching("AppleACPIPlatformExpert");
    if (!dict) {
        SYSLOG("ec0", "WTF? Failed to create matching dictionary");
        isTACHAvailable = false;
        return;
    }

    auto acpi = IOService::waitForMatchingService(dict);
    dict->release();

    if (!acpi) {
        SYSLOG("ec0", "WTF? No ACPI");
        isTACHAvailable = false;
        return;
    }

    acpi->release();

    dict = IOService::nameMatching("PNP0C09");
    if (!dict) {
        SYSLOG("ec0", "WTF? Failed to create matching dictionary");
        isTACHAvailable = false;
        return;
    }

    auto deviceIterator = IOService::getMatchingServices(dict);
    dict->release();

    if (!deviceIterator) {
        SYSLOG("ec0", "No iterator");
        isTACHAvailable = false;
        return;
    }

    ec0Device = OSDynamicCast(IOACPIPlatformDevice, deviceIterator->getNextObject());
    deviceIterator->release();

    if (!ec0Device) {
        SYSLOG("ec0", "PNP0C09 device not found");
        isTACHAvailable = false;
        return;
    }

    // Note: must NOT be a single "||" expression. With short-circuit
    // evaluation a missing TACH would skip refreshFan() entirely, and then
    // the WMI fan probe (which lives in refreshFan) would never run.
    if (ec0Device->validateObject("TACH") != kIOReturnSuccess) {
        SYSLOG("ec0", "No TACH method on EC0 device");
        isTACHAvailable = false;
    }

    // Do not bail out on a missing or broken TACH: refreshFan() falls back to
    // the firmware WMI fan device (ATKD.WMNB / DSTS), which is the reliable
    // path on boards where TACH() returns nonsense or does not exist.
    if (!refreshFan()) {
        SYSLOG("ec0", "No readable fan on EC0 device");
    }

    SYSLOG("ec0", "Found EC0 Device %s", ec0Device->getName());
}

void AsusSMC::initBattery() {
    // Battery Health was introduced in 10.15.5
    // Check if we're on 10.15.5+
    if (getKernelVersion() < KernelVersion::Catalina || (getKernelVersion() == KernelVersion::Catalina && getKernelMinorVersion() < 5)) {
        return;
    }

    isBatteryRSOCAvailable = wmi_dev_is_present(ASUS_WMI_DEVID_RSOC);
    if (isBatteryRSOCAvailable) {
        toggleBatteryConservativeMode(true);
    }
}

void AsusSMC::initVirtualKeyboard() {
    // Route the KeyboardBacklight collection to this driver before the device
    // starts, so a write from macOS is never dropped for lack of a sink.
    VirtualAppleKeyboard::setBacklightSink(virtualKeyboardBacklightSink, this);

    kbdDevice = new VirtualAppleKeyboard;

    if (!kbdDevice || !kbdDevice->init() || !kbdDevice->attach(this) || !kbdDevice->start(this)) {
        OSSafeReleaseNULL(kbdDevice);
        SYSLOG("vkbd", "Failed to init VirtualAppleKeyboard");
    }
}

void AsusSMC::virtualKeyboardBacklightSink(const uint8_t *bytes, uint32_t length, void *context) {
    AsusSMC *self = static_cast<AsusSMC *>(context);
    if (self) {
        self->handleVirtualKeyboardBacklight(bytes, length);
    }
}

void AsusSMC::handleVirtualKeyboardBacklight(const uint8_t *bytes, uint32_t length) {
    if (!bytes || length < 2) {
        return;
    }

    // Payload layout of the KeyboardBacklight collection is not documented.
    // Byte 0 is the report id, so the first data byte is the only sane
    // candidate: taken as-is when it already fits the hardware range,
    // otherwise scaled from a 0..255 range. The hex dump that setReport
    // publishes shows the real layout once the slider is used.
    uint32_t raw = bytes[1];
    uint8_t level;
    if (raw <= ASUS_KBD_BACKLIGHT_MAX) {
        level = (uint8_t)raw;
    } else {
        level = (uint8_t)(raw * ASUS_KBD_BACKLIGHT_MAX / 255);
    }

    kbdLevel = level;
    kbdDimmedForDisplay = false;

    VirtualAppleKeyboard::reportedLevel = (uint8_t)(level * 10);

    int ret = sendKbdBacklightViaHID(level);
    SYSLOG("kbd", "HID backlight write: level=%u raw=%u (ret=0x%X)",
           (unsigned)level, (unsigned)raw, (unsigned)ret);
    setProperty("AsusSMCHIDBacklight", "applied");
}

void AsusSMC::startATKDevice() {
    // Check direct ACPI messaging support
    if (atkDevice->validateObject("DMES") == kIOReturnSuccess) {
        DBGLOG("atk", "Direct ACPI message is supported");
        setProperty("IsDirectACPIMessagingSupported", kOSBooleanTrue);
        directACPImessaging = true;
    }

    // Check keyboard backlight support.
    //
    // Two shapes exist in the wild:
    //   - a dedicated SKBV method (older Intel ATK DSDTs)
    //   - the generic WMI device 0x00050021, whose DEVS branch calls
    //     EC0.SLKB (GA503QM and similar AMD ROG boards)
    // Only checking SKBV meant the second group was always reported as
    // unsupported, even though the write path works.
    if (atkDevice->validateObject("SKBV") == kIOReturnSuccess) {
        SYSLOG("atk", "Keyboard backlight is supported (SKBV)");
        hasKeyboardBacklight = true;
    } else if (pci0Ec0HasKeyboardBacklight()) {
        SYSLOG("atk", "Keyboard backlight is supported (WMI 0x%08X)",
               ASUS_WMI_DEVID_KEYBOARD_BACKLIGHT);
        hasKeyboardBacklight = true;
        keyboardBacklightViaWMI = true;
    } else {
        hasKeyboardBacklight = false;
        DBGLOG("atk", "Keyboard backlight is not supported");
    }
    setProperty("IsKeyboardBacklightSupported", hasKeyboardBacklight);
    setProperty("IsKeyboardBacklightViaWMI", keyboardBacklightViaWMI);

    // Mirror the whole probe result into the IORegistry.
    //
    // Lilu's SYSLOG goes through IOLog, which does not reliably reach the
    // unified log on current macOS, so `log show` can come back empty even
    // when the driver is running fine. IORegistry properties always work:
    //
    //   ioreg -rn AsusSMC -l | grep AsusSMC
    {
        char probe[224];
        snprintf(probe, sizeof(probe),
                 "kbd=%d viaWMI=%d direct=%d tach=%d fan0=%d fan1=%d "
                 "fanCtrl=%d boost=%d thermal=%d",
                 hasKeyboardBacklight ? 1 : 0,
                 keyboardBacklightViaWMI ? 1 : 0,
                 directACPImessaging ? 1 : 0,
                 isTACHAvailable ? 1 : 0,
                 isFanPresent0 ? 1 : 0,
                 isFanPresent1 ? 1 : 0,
                 isFanCtrlPresent ? 1 : 0,
                 isFanBoostPresent ? 1 : 0,
                 isThermalPolicyPresent ? 1 : 0);
        setProperty("AsusSMCProbe", probe);
        SYSLOG("atk", "probe summary: %s", probe);
    }

    // Light the keyboard at boot.
    //
    // Once the HID path started working, the previous build showed the
    // keyboard going dark at startup: macOS writes its restored level to LKSB
    // and, with the old broken mapping, that became level 0. The mapping is
    // fixed, but a deliberate default here means the keyboard is never left
    // dark just because the stored preference is 0. macOS may overwrite it a
    // moment later, which is fine.
    if (hasKeyboardBacklight) {
        // Publish the counters before anything else so a zero reading is
        // visible rather than absent.
        publishLKSAccess();

        int kr = sendKbdBacklightViaHID(2);
        char rec[96];
        snprintf(rec, sizeof(rec), "boot default level=2 hidRet=0x%X", (unsigned)kr);
        setProperty("AsusSMCBootBacklight", rec);
        SYSLOG("kbd", "%s", rec);
    }

    // Turn on ALS sensor
    toggleALS(true);
    isALSEnabled = true;
    setProperty("IsALSEnabled", isALSEnabled);
    SYSLOG("atk", "ALS is turned on at boot");
}

bool AsusSMC::refreshALS(bool post) {
    if (!alsDevice) {
        return false;
    }

    IOReturn ret = kIOReturnSuccess;
    uint32_t lux = 150;

    if (isALSEnabled) {
        ret = alsDevice->evaluateInteger("_ALI", &lux);
        if (ret != kIOReturnSuccess) {
            lux = 0xFFFFFFFF; // ACPI invalid
        }
    }

    atomic_store_explicit(&currentLux, lux, memory_order_release);

    if (post) {
        VirtualSMCAPI::postInterrupt(SmcEventALSChange);
        poller->setTimeoutMS(SensorUpdateTimeoutMS);
    }

    DBGLOG("als", "refreshALS lux %u", lux);

    return ret == kIOReturnSuccess;
}

bool AsusSMC::refreshFan() {
    // Probe what this machine actually exposes the first time round.
    if (!isFanProbed) {
        isFanProbed = true;

        isFanPresent0 = wmi_dev_is_present(ASUS_WMI_DEVID_CPU_FAN_CTRL);
        isFanPresent1 = wmi_dev_is_present(ASUS_WMI_DEVID_CPU_FAN2_CTRL);
        isFanCtrlPresent = wmi_dev_is_present(ASUS_WMI_DEVID_FAN_CTRL);
        isFanBoostPresent = wmi_dev_is_present(ASUS_WMI_DEVID_FAN_BOOST_MODE);
        isThermalPolicyPresent = wmi_dev_is_present(ASUS_WMI_DEVID_THROTTLE_THERMAL_POLICY);

        DBGLOG("fan", "probe: fan0 %d fan1 %d fan_ctrl %d boost %d thermal %d",
               isFanPresent0, isFanPresent1, isFanCtrlPresent, isFanBoostPresent,
               isThermalPolicyPresent);

        // TACH is only trustworthy if it also reports something sane. On a
        // lot of boards it does not, so prefer the WMI fan device when it
        // is present, which it is on every ATK laptop we care about.
        if (isFanPresent0) {
            isTACHAvailable = false;
        }
    }

    if (isFanPresent0 || isFanPresent1) {
        return refreshFanAll();
    }

    // Fall back to the old behaviour (TACH or a single WMI read).
    uint32_t speed = 10000;

    if (isTACHAvailable) {
        OSNumber *arg = OSNumber::withNumber(static_cast<uint32_t>(0), 32);
        IOReturn ret = ec0Device->evaluateInteger("TACH", &speed, (OSObject **)&arg, 1);
        arg->release();

        if (ret != kIOReturnSuccess) {
            DBGLOG("fan", "read fan speed using TACH failed");
            speed = 10000;
        }
    } else {
        int ret = wmi_get_devstate(ASUS_WMI_DEVID_CPU_FAN_CTRL);
        if (ret == -1) {
            DBGLOG("fan", "read fan speed using WMI failed");
            speed = 10000;
        } else {
            speed = (ret & 0xffff) * 100;
        }
    }

    atomic_store_explicit(&currentFanSpeed, speed, memory_order_release);

    DBGLOG("fan", "refreshFan speed %u", speed);

    return speed != 10000;
}

bool AsusSMC::fanDevIsPresent(uint32_t dev_id) {
    int status = wmi_get_devstate(dev_id);
    if (status == -1 || status == (int)ASUS_WMI_UNSUPPORTED_METHOD) {
        return false;
    }
    return (status & ASUS_WMI_DSTS_PRESENCE_BIT) != 0;
}

int AsusSMC::getFanSpeed(uint8_t index) {
    uint32_t dev_id = index ? ASUS_WMI_DEVID_CPU_FAN2_CTRL : ASUS_WMI_DEVID_CPU_FAN_CTRL;

    // Same decoding asus-wmi.c uses for fan1_input: the low half is the
    // fan counter in units of 100 RPM, the high half only carries flags.
    int status = wmi_evaluate_method(ASUS_WMI_METHODID_DSTS, dev_id, 0);
    if (status == -1 || status == (int)ASUS_WMI_UNSUPPORTED_METHOD) {
        return -1;
    }
    if ((status & ASUS_WMI_DSTS_PRESENCE_BIT) == 0) {
        return -1;
    }

    return (int)((status & 0xffff) * 100);
}

bool AsusSMC::refreshFanAll() {
    int s0 = isFanPresent0 ? getFanSpeed(0) : -1;
    int s1 = isFanPresent1 ? getFanSpeed(1) : -1;

    if (s0 < 0 && s1 < 0) {
        atomic_store_explicit(&fanValid, 0, memory_order_release);
        DBGLOG("fan", "refreshFanAll: no readable fan");
        return false;
    }

    // Do not trust a single failed read, only report invalid when both fail
    if (s0 >= 0) {
        atomic_store_explicit(&fanSpeed0, (uint16_t)s0, memory_order_release);
    }
    if (s1 >= 0) {
        atomic_store_explicit(&fanSpeed1, (uint16_t)s1, memory_order_release);
    }

    // Keep the legacy single-fan key pointing at the CPU fan
    if (s0 >= 0) {
        atomic_store_explicit(&currentFanSpeed, (uint16_t)s0, memory_order_release);
    }

    atomic_store_explicit(&fanValid, 1, memory_order_release);

    DBGLOG("fan", "refreshFanAll: fan0 %d fan1 %d", s0, s1);

    return true;
}

int AsusSMC::setFanMode(uint8_t index, uint32_t mode) {
    if (index > 1) {
        return -1;
    }
    if (index == 0 && !isFanPresent0) {
        return -1;
    }
    if (index == 1 && !isFanPresent1) {
        return -1;
    }

    uint32_t dev_id = index ? ASUS_WMI_DEVID_CPU_FAN2_CTRL : ASUS_WMI_DEVID_CPU_FAN_CTRL;
    uint32_t value;

    switch (mode) {
        case ASUS_FAN_CTRL_FULLSPEED:
            // DEVS value 1 sets bit 0x40 in the EC fan-control page, which is
            // the firmware's own "full speed" state. Verified on hardware:
            // fan0 goes from ~2400 to ~6800 RPM and comes back on value 0.
            value = 1;
            break;

        case ASUS_FAN_CTRL_AUTO:
            // DEVS value 0 clears the bit again, EC curve takes over.
            value = 0;
            break;

        default:
            // There is no per-fan manual PWM on boards without the
            // 0x00110012 AGFN device, so MANUAL is not offered here.
            return -1;
    }

    int ret = wmi_evaluate_method(ASUS_WMI_METHODID_DEVS, dev_id, value);
    if (ret == -1 || ret == (int)ASUS_WMI_UNSUPPORTED_METHOD) {
        DBGLOG("fan", "setFanMode: DEVS failed for fan %u", index);
        return -1;
    }

    atomic_store_explicit(index ? &fanMode1 : &fanMode0,
                          mode == ASUS_FAN_CTRL_FULLSPEED ? ASUS_FAN_CTRL_FULLSPEED : ASUS_FAN_CTRL_AUTO,
                          memory_order_release);

    DBGLOG("fan", "setFanMode: fan %u -> %u (ret %d)", index, mode, ret);

    return 0;
}

int AsusSMC::setThermalProfile(uint32_t profile) {
    // Reject anything the DSDT does not handle. Its If/ElseIf chain only
    // covers 0, 1 and 2; on any other value Local0 keeps its previous
    // contents and gets written to the EC, which on a GA503QM was observed
    // to spin both fans up to ~5000 RPM with no way to predict the result.
    // Never let an out-of-range value reach DEVS.
    if (profile != ASUS_THERMAL_POLICY_BALANCED &&
        profile != ASUS_THERMAL_POLICY_PERFORMANCE &&
        profile != ASUS_THERMAL_POLICY_SILENT) {
        DBGLOG("fan", "setThermalProfile: refusing invalid profile %u", profile);
        return -1;
    }

    if (!isThermalPolicyPresent) {
        DBGLOG("fan", "setThermalProfile: 0x00120075 not present on this board");
        return -1;
    }

    int ret = wmi_evaluate_method(ASUS_WMI_METHODID_DEVS,
                                  ASUS_WMI_DEVID_THROTTLE_THERMAL_POLICY,
                                  profile);
    if (ret == -1 || ret == (int)ASUS_WMI_UNSUPPORTED_METHOD) {
        DBGLOG("fan", "setThermalProfile: DEVS failed for profile %u", profile);
        return -1;
    }

    // AC returns 1 on success for this device.
    if (ret != 1) {
        DBGLOG("fan", "setThermalProfile: unexpected DEVS result %d", ret);
        return -1;
    }

    atomic_store_explicit(&thermalProfile, profile, memory_order_release);

    DBGLOG("fan", "setThermalProfile: -> %u (silent=2 bal=0 perf=1)", profile);

    return 0;
}

uint32_t AsusSMC::getThermalProfile() {
    // The DSDT answers DSTS for 0x00120075 with a hardcoded 0x00010003 and
    // never reports the live mode, so reading it back tells us nothing.
    // Report what we last wrote instead; on a fresh boot that is the
    // balanced default, which matches the firmware's own power-on state.
    return atomic_load_explicit(&thermalProfile, memory_order_acquire);
}

void AsusSMC::handleMessage(int code) {
    // Record every notification that reaches us.
    //
    // This answers a question the DSDT analysis cannot: the illumination keys
    // (0xC4 / 0xC5) are only produced by EC0.KBLD / KBLU, and those are called
    // exclusively from the DEVS 0x00100021 branch, i.e. they fire when a
    // driver tells ACPI about the key - not when the EC sees it. So the Fn
    // brightness keys very likely never reach this method as ACPI events, but
    // that is worth confirming on the real machine:
    //
    //   ioreg -rn AsusSMC -l | grep AsusSMCKey
    //   (press Fn+F2 / Fn+F3 first)
    lastKbdKey = (uint32_t)code;
    kbdKeyCount++;
    {
        char rec[64];
        snprintf(rec, sizeof(rec), "last=0x%X count=%u", (unsigned)code,
                 (unsigned)kbdKeyCount);
        setProperty("AsusSMCKey", rec);
    }

    switch (code) {
        case 0x57: // AC disconnected
        case 0x58: // AC connected
            // ignore silently
            break;

        case 0x30: // Volume up
            dispatchCSMRReport(kHIDUsage_Csmr_VolumeIncrement);
            break;

        case 0x31: // Volume down
            dispatchCSMRReport(kHIDUsage_Csmr_VolumeDecrement);
            break;

        case 0x32: // Mute
            dispatchCSMRReport(kHIDUsage_Csmr_Mute);
            break;

        // Media buttons
        case 0x40:
        case 0x8A:
            dispatchCSMRReport(kHIDUsage_Csmr_ScanPreviousTrack);
            break;

        case 0x41:
        case 0x82:
            dispatchCSMRReport(kHIDUsage_Csmr_ScanNextTrack);
            break;

        case 0x45:
        case 0x5C:
            dispatchCSMRReport(kHIDUsage_Csmr_PlayOrPause);
            break;

        case 0x33: // hardwired On
        case 0x34: // hardwired Off
        case 0x35: // Soft Event, Fn + F7
            displayOff();
            break;

        case 0x61: // Video Mirror
            dispatchTCReport(kHIDUsage_AV_TopCase_VideoMirror);
            break;

        case 0x6B: // Fn + F9, Touchpad On/Off
            toggleTouchpad();
            break;

        case 0x5E:
            letSleep();
            break;

        case 0x7A: // Fn + A, ALS Sensor
            // We should really do this in userspace
            // (e.g. "Automatically adjust brightness")
            isALSEnabled = !isALSEnabled;
            setProperty("IsALSEnabled", isALSEnabled);
            break;

        case 0x7D: // Airplane mode
            toggleAirplaneMode();
            break;

        case 0xC6:
        case 0xC7: // ALS Notifcations
            // ignore
            break;

        case 0xC5: // Keyboard Backlight Down
            if (hasKeyboardBacklight) {
                dispatchTCReport(kHIDUsage_AV_TopCase_IlluminationDown);
            }
            break;

        case 0xC4: // Keyboard Backlight Up
            if (hasKeyboardBacklight) {
                dispatchTCReport(kHIDUsage_AV_TopCase_IlluminationUp);
            }
            break;

        default:
            if (code >= NOTIFY_BRIGHTNESS_DOWN_MIN && code<= NOTIFY_BRIGHTNESS_DOWN_MAX) // Brightness Down
                dispatchTCReport(kHIDUsage_AV_TopCase_BrightnessDown);
            else if (code >= NOTIFY_BRIGHTNESS_UP_MIN && code<= NOTIFY_BRIGHTNESS_UP_MAX) // Brightness Up
                dispatchTCReport(kHIDUsage_AV_TopCase_BrightnessUp);
            break;
    }

    DBGLOG("atk", "Received key %d(0x%x)", code, code);
}

void AsusSMC::letSleep() {
    kev.sendMessage(kDaemonSleep, 0, 0);
}

void AsusSMC::toggleAirplaneMode() {
    kev.sendMessage(kDaemonAirplaneMode, 0, 0);
}

void AsusSMC::toggleTouchpad() {
    dispatchMessage(kKeyboardGetTouchStatus, &isTouchpadEnabled);
    isTouchpadEnabled = !isTouchpadEnabled;
    dispatchMessage(kKeyboardSetTouchStatus, &isTouchpadEnabled);

    if (isTouchpadEnabled) {
        setProperty("IsTouchpadEnabled", true);
        DBGLOG("atk", "Enabled Touchpad");
    } else {
        setProperty("IsTouchpadEnabled", false);
        DBGLOG("atk", "Disabled Touchpad");
    }
}

void AsusSMC::toggleALS(bool state) {
    if (wmi_evaluate_method(ASUS_WMI_METHODID_DEVS, ASUS_WMI_DEVID_ALS_ENABLE, state ? 1 : 0) == -1) {
        SYSLOG("atk", "Failed to %s ALSC", state ? "enable" : "disable");
    } else {
        DBGLOG("atk", "ALS is %s", state ? "enabled" : "disabled");
    }
}

void AsusSMC::toggleBatteryConservativeMode(bool state) {
    if (!isBatteryRSOCAvailable) {
        DBGLOG("batt", "RSOC unavailable");
        return;
    }

    if (wmi_evaluate_method(ASUS_WMI_METHODID_DEVS, ASUS_WMI_DEVID_RSOC, state ? 80 : 100) != 1) {
        SYSLOG("batt", "Failed to %s battery conservative mode", state ? "enable" : "disable");
    } else {
        DBGLOG("batt", "Battery conservative mode is %s", state ? "enabled" : "disabled");
        setProperty("BatteryConservativeMode", state);
    }
}

void AsusSMC::displayOff() {
    if (isPanelBackLightOn) {
        // Read Panel brigthness value to restore later with backlight toggle
        readPanelBrightnessValue();

        dispatchTCReport(kHIDUsage_AV_TopCase_BrightnessDown, 16);
    } else {
        dispatchTCReport(kHIDUsage_AV_TopCase_BrightnessUp, panelBrightnessLevel);
    }

    isPanelBackLightOn = !isPanelBackLightOn;
}

/**
 *  Keep the keyboard backlight in step with the panel.
 *
 *  macOS will not do this for us on this machine. Its keyboard-brightness
 *  support is per-model, and the SMC dumps show LKSB/LKSS only on 2015 and
 *  older Macs - the 2016 top-case generation moved keyboard backlight off the
 *  SMC entirely. This driver reports SMBIOS MacBookPro16,2 (2020), so macOS
 *  never touches LKSB and therefore never blanks the keyboard when the screen
 *  goes off. The result is a keyboard that stays lit through display sleep.
 *
 *  Polled from the existing sensor timer rather than hooked into power
 *  management, which keeps the change contained: we only ever call the HID
 *  write path that is already proven to work.
 */
void AsusSMC::followDisplayPower() {
    if (!hasKeyboardBacklight) {
        return;
    }

    uint32_t wranglerState = 0;
    bool haveWrangler = readDisplayWranglerState(&wranglerState);

    bool havePanel = readPanelBrightnessValue();

    // Run the power chain probe once, after the first real read attempt, so it
    // describes the state the reader actually saw rather than a startup guess.
    if (!powerProbeDone) {
        powerProbeDone = true;
        probePowerRegistry();
    }

    // Retry the boot-time level. It fails at start() because AsusSMC comes up
    // long before the USB keyboard enumerates (kIOReturnNotReady), and this is
    // the first place where the device is reliably available.
    if (!kbdBootApplied) {
        if (sendKbdBacklightViaHID(kbdLevel) == kIOReturnSuccess) {
            kbdBootApplied = true;
        }
    }

    // Everything is recorded rather than acted on blindly.
    //
    // The display cannot be observed from a shell while it is asleep - typing
    // to read ioreg wakes it - so the driver keeps a running record of what it
    // saw. A single read afterwards then answers whether the signal moved at
    // all, which is the only way to calibrate the semantics:
    //
    //   ioreg -rn AsusSMC -l | grep AsusSMCPanelTrace
    if (haveWrangler) {
        if (!traceSeen) {
            traceWranglerMin = traceWranglerMax = wranglerState;
            traceSeen = true;
        } else {
            if (wranglerState < traceWranglerMin) traceWranglerMin = wranglerState;
            if (wranglerState > traceWranglerMax) traceWranglerMax = wranglerState;
        }
    }
    if (havePanel) {
        if (traceBrightnessMin < 0) {
            traceBrightnessMin = traceBrightnessMax = (int)panelBrightnessLevel;
        } else {
            if ((int)panelBrightnessLevel < traceBrightnessMin) traceBrightnessMin = (int)panelBrightnessLevel;
            if ((int)panelBrightnessLevel > traceBrightnessMax) traceBrightnessMax = (int)panelBrightnessLevel;
        }
    }

    char rec[192];
    snprintf(rec, sizeof(rec),
             "wrangler=%u panelBrightness=%u dimmed=%d level=%u trace=%d,%d,%d,%d",
             (unsigned)wranglerState,
             havePanel ? (unsigned)panelBrightnessLevel : 0u,
             kbdDimmedForDisplay ? 1 : 0,
             (unsigned)kbdLevel,
             traceWranglerMin, traceWranglerMax, traceBrightnessMin, traceBrightnessMax);
    setProperty("AsusSMCPanel", rec);

    char trace[128];
    snprintf(trace, sizeof(trace), "wrangler=%d..%d brightness=%d..%d dimmed=%d",
             traceWranglerMin, traceWranglerMax,
             traceBrightnessMin, traceBrightnessMax,
             kbdDimmedForDisplay ? 1 : 0);
    setProperty("AsusSMCPanelTrace", trace);

    if (!haveWrangler && !havePanel) {
        return;
    }

    // Decide from the wrangler when it is readable, otherwise from the panel.
    //
    // Measured on the machine: with the display awake CurrentPowerState is 4,
    // and after `pmset displaysleepnow` it drops to 1 (DevicePowerState 1,
    // CapabilityFlags 0). So the "display is not fully on" states are the low
    // ordinals; anything at or above 2 is treated as awake.
    //
    // Two consecutive reads are required before blanking, so a single
    // transient value cannot turn the keyboard off.
    bool displayOff = haveWrangler ? (wranglerState <= 1)
                                   : (panelBrightnessLevel == 0);

    if (displayOff) {
        offStreak++;
    } else {
        offStreak = 0;
    }

    if (offStreak >= 2 && !kbdDimmedForDisplay) {
        int kr = sendKbdBacklightViaHID(0);
        kbdDimmedForDisplay = true;
        SYSLOG("kbd", "display off -> keyboard backlight off (ret=0x%X)", (unsigned)kr);
    } else if (!displayOff && kbdDimmedForDisplay && offStreak == 0) {
        int kr = sendKbdBacklightViaHID(kbdLevel);
        kbdDimmedForDisplay = false;
        SYSLOG("kbd", "display on -> keyboard backlight %u (ret=0x%X)",
               (unsigned)kbdLevel, (unsigned)kr);
    }
}

/**
 *  Read the current display power state.
 *
 *  Measured on this machine: CurrentPowerState is 4 with the display awake and
 *  1 after `pmset displaysleepnow`.
 *
 *  The property is not on the IODisplayWrangler service itself - reading it
 *  there always returned nothing, while `ioreg -r` shows the value because it
 *  recurses. So the service is searched first, then its children.
 */
static bool readPowerStateFrom(IOService *svc, uint32_t *state, const char **source) {
    if (!svc) {
        return false;
    }
    OSDictionary *pm = OSDynamicCast(OSDictionary, svc->getProperty("IOPowerManagement"));
    if (!pm) {
        return false;
    }
    OSNumber *cur = OSDynamicCast(OSNumber, pm->getObject("CurrentPowerState"));
    if (!cur) {
        return false;
    }
    *state = cur->unsigned32BitValue();
    if (source) {
        *source = svc->getName();
    }
    return true;
}

bool AsusSMC::readDisplayWranglerState(uint32_t *state) {
    if (!state) {
        return false;
    }

    if (!wranglerEntry) {
        OSDictionary *matching = IOService::serviceMatching("IODisplayWrangler");
        if (!matching) {
            return false;
        }
        // waitForMatchingService hands back an owned reference.
        IOService *svc = IOService::waitForMatchingService(matching, 100000000ULL);
        matching->release();
        if (!svc) {
            return false;
        }
        wranglerEntry = svc;
        setProperty("AsusSMCDisplayFound", svc->getName());
    }

    const char *source = nullptr;

    // 0. The entry that worked last time.
    if (powerEntry && readPowerStateFrom(powerEntry, state, &source)) {
        return true;
    }

    // 1. The wrangler itself, then its subtree. `ioreg -r` shows the value
    //    under the wrangler, which is why it was assumed to be on the service.
    if (readPowerStateFrom(wranglerEntry, state, &source)) {
        powerEntry = wranglerEntry;
        setProperty("AsusSMCPowerSource", source ? source : "wrangler");
        return true;
    }

    if (trySubtree(wranglerEntry, 2, state, &source)) {
        setProperty("AsusSMCPowerSource", source ? source : "wrangler-child");
        return true;
    }

    // 2. The display service found for the brightness value; AppleBacklightDisplay
    //    carries the graphics properties, so it may carry the power state too.
    if (panelEntry) {
        IOService *panel = OSDynamicCast(IOService, panelEntry);
        if (readPowerStateFrom(panel, state, &source)) {
            powerEntry = panel;
            setProperty("AsusSMCPowerSource", source ? source : "panel");
            return true;
        }
        if (trySubtree(panel, 1, state, &source)) {
            setProperty("AsusSMCPowerSource", source ? source : "panel-child");
            return true;
        }
    }

    // 3. Last resort: walk the whole registry looking for a power management
    //    dictionary. The source name is reported so the choice can be judged
    //    rather than trusted, together with how many candidates were seen at
    //    all - if this reports none seen=0 the walk itself is broken, which is
    //    exactly what the previous version hid.
    {
        IORegistryIterator *iter =
            IORegistryIterator::iterateOver(gIOServicePlane, kIORegistryIterateRecursively);
        if (iter) {
            IOService *first = nullptr;
            const char *firstName = nullptr;
            uint32_t firstState = 0;
            IOService *displayish = nullptr;
            const char *displayishName = nullptr;
            uint32_t displayishState = 0;
            uint32_t seen = 0;

            while (OSObject *obj = iter->getNextObject()) {
                // The recursive walk starts at the plane root, which is not an
                // IOService. The previous version used the cast as the loop
                // condition, so the first such entry ended the walk and the
                // scan reported nothing at all.
                IOService *svc = OSDynamicCast(IOService, obj);
                if (!svc) {
                    continue;
                }

                uint32_t v = 0;
                const char *n = nullptr;
                if (!readPowerStateFrom(svc, &v, &n)) {
                    continue;
                }
                seen++;

                if (!first) {
                    first = svc;
                    firstName = n;
                    firstState = v;
                }

                // Prefer a display related entry over the first thing that
                // happens to answer: plenty of services carry a power
                // management dictionary.
                const char *nm = svc->getName();
                if (nm && (strstr(nm, "Display") || strstr(nm, "Wrangler"))) {
                    displayish = svc;
                    displayishName = n;
                    displayishState = v;
                    break;
                }
            }
            iter->release();

            char diag[96];
            if (displayish || first) {
                IOService *pick = displayish ? displayish : first;
                const char *pickName = displayish ? displayishName : firstName;
                uint32_t pickState = displayish ? displayishState : firstState;

                pick->retain();
                powerEntry = pick;
                *state = pickState;

                snprintf(diag, sizeof(diag), "%s state=%u seen=%u",
                         pickName ? pickName : "?", pickState, seen);
                setProperty("AsusSMCPowerSource", diag);
                return true;
            }

            snprintf(diag, sizeof(diag), "none seen=%u", seen);
            setProperty("AsusSMCPowerSource", diag);
        }
    }

    return false;
}

/**
 *  One-shot self diagnosis of the display power chain.
 *
 *  Four rounds were spent guessing which registry entry holds the display
 *  power state, and the read kept reporting nothing. This reports every hop
 *  separately so the broken link is visible in one boot instead of being
 *  inferred:
 *
 *   wPM   does IODisplayWrangler carry IOPowerManagement, of what class,
 *         and does it contain CurrentPowerState
 *   pPM   the same for the panel entry (AppleBacklightDisplay)
 *   scan  how many services the registry walk visited, how many carry
 *         IOPowerManagement at all, how many carry CurrentPowerState, and
 *         the first name that had one
 */
void AsusSMC::probePowerRegistry() {
    char diag[192];

    bool wPM = false, wCur = false, pPM = false, pCur = false;
    const char *wType = "null", *pType = "null";

    if (wranglerEntry) {
        if (OSObject *o = wranglerEntry->getProperty("IOPowerManagement")) {
            wPM = true;
            wType = o->getMetaClass()->getClassName();
            if (OSDictionary *d = OSDynamicCast(OSDictionary, o)) {
                wCur = d->getObject("CurrentPowerState") != nullptr;
            }
        }
    }
    if (panelEntry) {
        if (OSObject *o = panelEntry->getProperty("IOPowerManagement")) {
            pPM = true;
            pType = o->getMetaClass()->getClassName();
            if (OSDictionary *d = OSDynamicCast(OSDictionary, o)) {
                pCur = d->getObject("CurrentPowerState") != nullptr;
            }
        }
    }

    uint32_t scanned = 0, withPM = 0, withCur = 0;
    char firstCurName[48] = "-";

    if (IORegistryIterator *iter =
            IORegistryIterator::iterateOver(gIOServicePlane, kIORegistryIterateRecursively)) {
        while (OSObject *obj = iter->getNextObject()) {
            IOService *svc = OSDynamicCast(IOService, obj);
            if (!svc) {
                continue;
            }
            scanned++;

            OSObject *o = svc->getProperty("IOPowerManagement");
            if (!o) {
                continue;
            }
            withPM++;

            OSDictionary *d = OSDynamicCast(OSDictionary, o);
            if (!d || !d->getObject("CurrentPowerState")) {
                continue;
            }
            withCur++;

            if (firstCurName[0] == '-') {
                const char *n = svc->getName();
                snprintf(firstCurName, sizeof(firstCurName), "%s", n ? n : "?");
            }
        }
        iter->release();
    }

    snprintf(diag, sizeof(diag),
             "wPM=%d/%s/%d pPM=%d/%s/%d scan=%u/%u/%u first=%s",
             wPM ? 1 : 0, wType, wCur ? 1 : 0,
             pPM ? 1 : 0, pType, pCur ? 1 : 0,
             scanned, withPM, withCur, firstCurName);
    setProperty("AsusSMCPowerDiag", diag);
    SYSLOG("kbd", "power registry probe: %s", diag);
}

/**
 *  Walk an entry's descendants looking for a power management dictionary.
 *  Returns the entry that answered through `source`.
 */
bool AsusSMC::trySubtree(IOService *root, int depth, uint32_t *state, const char **source) {
    if (!root || depth <= 0) {
        return false;
    }

    OSIterator *iter = root->getChildIterator(gIOServicePlane);
    if (!iter) {
        return false;
    }
    iter->reset();

    IOService *child = nullptr;
    while (OSObject *obj = iter->getNextObject()) {
        IOService *c = OSDynamicCast(IOService, obj);
        if (!c) {
            continue;
        }
        if (readPowerStateFrom(c, state, source)) {
            child = c;
            break;
        }
    }
    iter->release();

    if (child) {
        return true;
    }

    // Recurse one level down. The iterator cannot be reused after release, so
    // the children are walked again.
    OSIterator *again = root->getChildIterator(gIOServicePlane);
    if (!again) {
        return false;
    }
    again->reset();
    bool found = false;
    while (OSObject *obj = again->getNextObject()) {
        IOService *c = OSDynamicCast(IOService, obj);
        if (c && trySubtree(c, depth - 1, state, source)) {
            found = true;
            break;
        }
    }
    again->release();
    return found;
}

int AsusSMC::checkBacklightEntry() {
    if (IORegistryEntry *bkl = IORegistryEntry::fromPath(backlightEntry)) {
        OSSafeReleaseNULL(bkl);
        return 1;
    } else {
        DBGLOG("atk", "Failed to find backlight entry for %s", backlightEntry);
        return 0;
    }
}

int AsusSMC::findBacklightEntry() {
    // Check for previous found backlight entry
    if (checkBacklightEntry()) {
        return 1;
    }

    snprintf(backlightEntry, sizeof(backlightEntry), "IOService:/AppleACPIPlatformExpert/PCI0@0/AppleACPIPCI/IGPU@2/AppleIntelFramebuffer@0/display0/AppleBacklightDisplay");
    if (checkBacklightEntry()) {
        return 1;
    }

    snprintf(backlightEntry, sizeof(backlightEntry), "IOService:/AppleACPIPlatformExpert/PCI0@0/AppleACPIPCI/GFX0@2/AppleIntelFramebuffer@0/display0/AppleBacklightDisplay");
    if (checkBacklightEntry()) {
        return 1;
    }

    char deviceName[5][5] = {"PEG0", "PEGP", "PEGR", "P0P2", "IXVE"};
    for (int i = 0; i < 5; i++) {
        snprintf(backlightEntry, sizeof(backlightEntry), "IOService:/AppleACPIPlatformExpert/PCI0@0/AppleACPIPCI/%s@1/IOPP/GFX0@0/NVDA,Display-A@0/NVDA/display0/AppleBacklightDisplay", deviceName[i]);
        if (checkBacklightEntry()) {
            return 1;
        }

        snprintf(backlightEntry, sizeof(backlightEntry), "IOService:/AppleACPIPlatformExpert/PCI0@0/AppleACPIPCI/%s@3/IOPP/GFX0@0/NVDA,Display-A@0/NVDATesla/display0/AppleBacklightDisplay", deviceName[i]);
        if (checkBacklightEntry()) {
            return 1;
        }

        snprintf(backlightEntry, sizeof(backlightEntry), "IOService:/AppleACPIPlatformExpert/PCI0@0/AppleACPIPCI/%s@10/IOPP/GFX0@0/NVDA,Display-A@0/NVDATesla/display0/AppleBacklightDisplay", deviceName[i]);
        if (checkBacklightEntry()) {
            return 1;
        }

        snprintf(backlightEntry, sizeof(backlightEntry), "IOService:/AppleACPIPlatformExpert/PCI0@0/AppleACPIPCI/%s@1/IOPP/display@0/NVDA,Display-A@0/NVDA/display0/AppleBacklightDisplay", deviceName[i]);
        if (checkBacklightEntry()) {
            return 1;
        }

        snprintf(backlightEntry, sizeof(backlightEntry), "IOService:/AppleACPIPlatformExpert/PCI0@0/AppleACPIPCI/%s@3/IOPP/display@0/NVDA,Display-A@0/NVDATesla/display0/AppleBacklightDisplay", deviceName[i]);
        if (checkBacklightEntry()) {
            return 1;
        }

        snprintf(backlightEntry, sizeof(backlightEntry), "IOService:/AppleACPIPlatformExpert/PCI0@0/AppleACPIPCI/%s@10/IOPP/display@0/NVDA,Display-A@0/NVDATesla/display0/AppleBacklightDisplay", deviceName[i]);
        if (checkBacklightEntry()) {
            return 1;
        }
    }

    return 0;
}

bool AsusSMC::readPanelBrightnessValue() {
    if (!findBacklightEntry()) {
        DBGLOG("atk", "GPU device not found");
        // The static paths above are Intel and NVIDIA specific. On an AMD
        // machine (this GA503QM with NootedRed) they all miss, so fall back
        // to searching the registry for whichever service actually publishes
        // the display brightness.
        if (!findPanelEntryBySearch()) {
            return false;
        }
    }

    IORegistryEntry *displayDeviceEntry = IORegistryEntry::fromPath(backlightEntry);
    if (!displayDeviceEntry && panelEntry) {
        displayDeviceEntry = panelEntry;
        displayDeviceEntry->retain();
    }

    bool ok = false;
    if (displayDeviceEntry) {
        if (OSDictionary *ioDisplayParaDict = OSDynamicCast(OSDictionary, displayDeviceEntry->getProperty("IODisplayParameters"))) {
            if (OSDictionary *brightnessDict = OSDynamicCast(OSDictionary, ioDisplayParaDict->getObject("brightness"))) {
                if (OSNumber *brightnessValue = OSDynamicCast(OSNumber, brightnessDict->getObject("value"))) {
                    panelBrightnessLevel = brightnessValue->unsigned32BitValue() / 64;
                    ok = true;
                    DBGLOG("atk", "Panel brightness level: %d", panelBrightnessLevel);
                } else {
                    DBGLOG("atk", "Failed to read brightness value");
                }
            } else {
                DBGLOG("atk", "Failed to find dictionary brightness");
            }
        } else {
            DBGLOG("atk", "Failed to find dictionary IODisplayParameters");
        }
    }
    OSSafeReleaseNULL(displayDeviceEntry);
    return ok;
}

/**
 *  Find the display service that publishes IODisplayParameters.brightness.
 *
 *  Only used when the hard-coded Intel/NVIDIA paths fail, and the result is
 *  cached, so the registry walk happens at most once.
 */
bool AsusSMC::findPanelEntryBySearch() {
    if (panelEntry) {
        return true;
    }

    IORegistryIterator *iter = IORegistryIterator::iterateOver(gIOServicePlane, kIORegistryIterateRecursively);
    if (!iter) {
        return false;
    }

    IORegistryEntry *found = nullptr;
    while (IORegistryEntry *entry = iter->getNextObject()) {
        OSDictionary *params = OSDynamicCast(OSDictionary, entry->getProperty("IODisplayParameters"));
        if (!params) {
            continue;
        }
        OSDictionary *brightness = OSDynamicCast(OSDictionary, params->getObject("brightness"));
        if (!brightness || !brightness->getObject("value")) {
            continue;
        }
        found = entry;
        break;
    }
    iter->release();

    if (found) {
        found->retain();
        panelEntry = found;
        setProperty("AsusSMCPanelFound", found->getName());
        return true;
    }

    setProperty("AsusSMCPanelFound", "none");
    return false;
}

IOReturn AsusSMC::postKeyboardInputReport(const void *report, uint32_t reportSize) {
    IOReturn result = kIOReturnError;

    if (!report || reportSize == 0) {
        return kIOReturnBadArgument;
    }

    if (kbdDevice) {
        if (auto buffer = IOBufferMemoryDescriptor::withBytes(report, reportSize, kIODirectionNone)) {
            result = kbdDevice->handleReport(buffer, kIOHIDReportTypeInput, kIOHIDOptionsTypeNone);
            buffer->release();
        }
    }

    return result;
}

void AsusSMC::dispatchCSMRReport(int code, int loop) {
    DBGLOG("atk", "Dispatched key %d(0x%x), loop %d time(s)", code, code, loop);
    while (loop--) {
        csmrreport.keys.insert(code);
        postKeyboardInputReport(&csmrreport, sizeof(csmrreport));
        csmrreport.keys.erase(code);
        postKeyboardInputReport(&csmrreport, sizeof(csmrreport));
    }
}

void AsusSMC::dispatchTCReport(int code, int loop) {
    DBGLOG("atk", "Dispatched key %d(0x%x), loop %d time(s)", code, code, loop);
    while (loop--) {
        tcreport.keys.insert(code);
        postKeyboardInputReport(&tcreport, sizeof(tcreport));
        tcreport.keys.erase(code);
        postKeyboardInputReport(&tcreport, sizeof(tcreport));
    }
}

void AsusSMC::registerNotifications() {
    auto *key = OSSymbol::withCString(kDeliverNotifications);
    auto *propertyMatch = propertyMatching(key, kOSBooleanTrue);

    IOServiceMatchingNotificationHandler notificationHandler = OSMemberFunctionCast(IOServiceMatchingNotificationHandler, this, &AsusSMC::notificationHandler);

    _publishNotify = addMatchingNotification(gIOFirstPublishNotification,
                                             propertyMatch,
                                             notificationHandler,
                                             this,
                                             0, 10000);

    _terminateNotify = addMatchingNotification(gIOTerminatedNotification,
                                               propertyMatch,
                                               notificationHandler,
                                               this,
                                               0, 10000);

    key->release();
    propertyMatch->release();
}

void AsusSMC::notificationHandlerGated(IOService *newService, IONotifier *notifier) {
    if (notifier == _publishNotify) {
        SYSLOG("notify", "Notification consumer published: %s", newService->getName());
        _notificationServices->setObject(newService);
    }

    if (notifier == _terminateNotify) {
        SYSLOG("notify", "Notification consumer terminated: %s", newService->getName());
        _notificationServices->removeObject(newService);
    }
}

bool AsusSMC::notificationHandler(void *refCon, IOService *newService, IONotifier *notifier) {
    command_gate->runAction(OSMemberFunctionCast(IOCommandGate::Action, this, &AsusSMC::notificationHandlerGated), newService, notifier);
    return true;
}

void AsusSMC::dispatchMessageGated(int *message, void *data) {
    OSCollectionIterator *i = OSCollectionIterator::withCollection(_notificationServices);

    if (i != NULL) {
        while (IOService *service = OSDynamicCast(IOService, i->getNextObject())) {
            service->message(*message, this, data);
        }
        i->release();
    }
}

void AsusSMC::dispatchMessage(int message, void *data) {
    command_gate->runAction(OSMemberFunctionCast(IOCommandGate::Action, this, &AsusSMC::dispatchMessageGated), &message, data);
}

void AsusSMC::registerVSMC() {
    vsmcNotifier = VirtualSMCAPI::registerHandler(vsmcNotificationHandler, this);

    ALSSensor sensor {ALSSensor::Type::Unknown7, true, 6, false};
    ALSSensor noSensor {ALSSensor::Type::NoSensor, false, 0, false};
    SMCALSValue::Value emptyValue;

    // LKSB / LKSS contents.
    //
    // Neither byte is documented anywhere: upstream's defaults are {0, 1} and
    // the struct fields are literally named unknown0/unknown1, so they are a
    // guess. Tested on the machine: disabling AsusSMC makes the System
    // Settings keyboard brightness control disappear entirely, which proves
    // macOS reads these keys to decide whether to offer it. The control is
    // then shown disabled, i.e. macOS finds the key but not an adjustable
    // range.
    //
    // The working hypothesis is that the two bytes carry {current, maximum}
    // and that a maximum of 1 means "one level", leaving nothing to adjust.
    // The defaults are therefore raised to a 16 step range.
    //
    // Every byte can be overridden from boot-args, so the encoding can be
    // swept without rebuilding:
    //   -asussmclkb0=0 -asussmclkb1=16 -asussmclks0=0 -asussmclks1=16
    SMCKBrdBLightValue::lkb lkb;
    lkb.val1 = 0;
    lkb.val2 = 16;

    SMCKBrdBLightValue::lks lks;
    lks.unknown0 = 0;
    lks.unknown1 = 16;

    {
        int32_t v = 0;
        if (PE_parse_boot_argn("asussmclkb0", &v, sizeof(v))) {
            lkb.val1 = (uint8_t)v;
        }
        if (PE_parse_boot_argn("asussmclkb1", &v, sizeof(v))) {
            lkb.val2 = (uint8_t)v;
        }
        if (PE_parse_boot_argn("asussmclks0", &v, sizeof(v))) {
            lks.unknown0 = (uint8_t)v;
        }
        if (PE_parse_boot_argn("asussmclks1", &v, sizeof(v))) {
            lks.unknown1 = (uint8_t)v;
        }

        char init[96];
        snprintf(init, sizeof(init), "lkb=%u,%u lks=%u,%u",
                 lkb.val1, lkb.val2, lks.unknown0, lks.unknown1);
        setProperty("AsusSMCInit", init);
        SYSLOG("kbd", "LKSB/LKSS initial values %s", init);
    }

    // Key group switches. What was actually registered is published as
    // AsusSMCKeys, because a boot-arg that silently fails to parse is
    // indistinguishable from one that was never added:
    //
    //   -asussmcnolks   leave LKSB and LKSS unregistered
    //   -asussmcnoalv   leave the whole ambient light group unregistered
    //                   (AL, ALI0, ALI1, ALRV, ALV0, ALV1)
    bool registerLKS = !checkKernelArgument("-asussmcnolks");
    bool registerALV = !checkKernelArgument("-asussmcnoalv");

    {
        char keys[48];
        snprintf(keys, sizeof(keys), "lks=%d alv=%d",
                 registerLKS ? 1 : 0, registerALV ? 1 : 0);
        setProperty("AsusSMCKeys", keys);
    }

    // -asussmcnoalv now covers the whole ambient light group, not just ALV0 and
    // ALV1. The two-boot experiment showed the System Settings panel survives
    // both switches, but AL, ALI0, ALI1 and ALRV were registered in both boots,
    // so they were never actually separated from anything:
    //
    //   boot A: lks=0 alv=1  -> panel present (ALV0/1 + AL/ALI0/ALI1/ALRV)
    //   boot B: lks=1 alv=0  -> panel present (AL/ALI0/ALI1/ALRV still on)
    //
    // ALI0 and ALI1 are the sensor descriptors: real MacBookPro11,4 reports
    // ALI0 = 07000600, which is exactly what this driver's
    // {Unknown7, true, 6, false} produces, so the panel is almost certainly
    // driven by the presence of these keys.
    if (registerALV) {
        VirtualSMCAPI::addKey(KeyAL, vsmcPlugin.data, VirtualSMCAPI::valueWithUint16(
            0, &forceBits,
            SMC_KEY_ATTRIBUTE_READ | SMC_KEY_ATTRIBUTE_WRITE));

        VirtualSMCAPI::addKey(KeyALI0, vsmcPlugin.data, VirtualSMCAPI::valueWithData(
            reinterpret_cast<const SMC_DATA *>(&sensor), sizeof(sensor), SmcKeyTypeAli, nullptr,
            SMC_KEY_ATTRIBUTE_READ | SMC_KEY_ATTRIBUTE_FUNCTION));

        VirtualSMCAPI::addKey(KeyALI1, vsmcPlugin.data, VirtualSMCAPI::valueWithData(
            reinterpret_cast<const SMC_DATA *>(&noSensor), sizeof(noSensor), SmcKeyTypeAli, nullptr,
            SMC_KEY_ATTRIBUTE_READ | SMC_KEY_ATTRIBUTE_FUNCTION));

        VirtualSMCAPI::addKey(KeyALRV, vsmcPlugin.data, VirtualSMCAPI::valueWithUint16(
            1, nullptr,
            SMC_KEY_ATTRIBUTE_READ));
    }

    if (registerALV) {
        VirtualSMCAPI::addKey(KeyALV0, vsmcPlugin.data, VirtualSMCAPI::valueWithData(
            reinterpret_cast<const SMC_DATA *>(&emptyValue), sizeof(emptyValue), SmcKeyTypeAlv, new SMCALSValue(&currentLux, &forceBits),
            SMC_KEY_ATTRIBUTE_READ | SMC_KEY_ATTRIBUTE_WRITE | SMC_KEY_ATTRIBUTE_FUNCTION));

        VirtualSMCAPI::addKey(KeyALV1, vsmcPlugin.data, VirtualSMCAPI::valueWithData(
            reinterpret_cast<const SMC_DATA *>(&emptyValue), sizeof(emptyValue), SmcKeyTypeAlv, nullptr,
            SMC_KEY_ATTRIBUTE_READ | SMC_KEY_ATTRIBUTE_WRITE | SMC_KEY_ATTRIBUTE_FUNCTION));
    }

    // LKSB / LKSS are registered read/write, not write-only.
    //
    // A previous revision made them write-only because the real Mac dumps
    // report "NOT READABLE, code 85" and the attribute byte there is 0x50
    // (WRITE | FUNCTION). That was a mistake, for two reasons:
    //
    //   * The System Settings keyboard brightness control is still displayed
    //     on this machine, greyed out. A machine with no keyboard backlight at
    //     all does not show that control, so macOS is detecting a backlight -
    //     and LKSB/LKSS are the only such interface this driver publishes. So
    //     macOS is looking at these keys after all.
    //   * If macOS reads the current level to render the slider, a key that
    //     cannot be read is exactly what would leave the slider disabled.
    //     "NOT READABLE" in the dump may also just be the dump tool lacking
    //     the access those keys want.
    //
    // Keeping READ also restores the read counters, which are the direct
    // evidence of whether macOS touches these keys at all.
    if (registerLKS) {
        VirtualSMCAPI::addKey(KeyLKSB, vsmcPlugin.data, VirtualSMCAPI::valueWithData(
            reinterpret_cast<const SMC_DATA *>(&lkb), sizeof(lkb), SmcKeyTypeLkb, new SMCKBrdBLightValue(this),
            SMC_KEY_ATTRIBUTE_READ | SMC_KEY_ATTRIBUTE_WRITE | SMC_KEY_ATTRIBUTE_FUNCTION));

        VirtualSMCAPI::addKey(KeyLKSS, vsmcPlugin.data, VirtualSMCAPI::valueWithData(
            reinterpret_cast<const SMC_DATA *>(&lks), sizeof(lks), SmcKeyTypeLks, new SMCKBrdBLightState(this),
            SMC_KEY_ATTRIBUTE_READ | SMC_KEY_ATTRIBUTE_WRITE | SMC_KEY_ATTRIBUTE_FUNCTION));
    }

    VirtualSMCAPI::addKey(KeyMSLD, vsmcPlugin.data, VirtualSMCAPI::valueWithUint8(
        0, nullptr,
        SMC_KEY_ATTRIBUTE_READ | SMC_KEY_ATTRIBUTE_WRITE | SMC_KEY_ATTRIBUTE_FUNCTION));

    VirtualSMCAPI::addKey(KeyFNum, vsmcPlugin.data, VirtualSMCAPI::valueWithUint8(
        isFanPresent1 ? 2 : 1, nullptr,
        SMC_KEY_ATTRIBUTE_CONST | SMC_KEY_ATTRIBUTE_READ));

    VirtualSMCAPI::addKey(KeyF0Ac, vsmcPlugin.data, VirtualSMCAPI::valueWithFp(
        0, SmcKeyTypeFpe2, new F0Ac(&fanSpeed0),
        SMC_KEY_ATTRIBUTE_READ | SMC_KEY_ATTRIBUTE_FUNCTION));

    // Plausible range for the CPU fan of this machine, so Macs Fan Control and
    // iStat can draw the gauge. These are constants: the real limits live in
    // the EC firmware and are not readable through DEVS/DSTS.
    VirtualSMCAPI::addKey(KeyF0Mn, vsmcPlugin.data, VirtualSMCAPI::valueWithFp(
        0, SmcKeyTypeFpe2, new FConstAc(kFanMinRPM),
        SMC_KEY_ATTRIBUTE_READ));
    VirtualSMCAPI::addKey(KeyF0Mx, vsmcPlugin.data, VirtualSMCAPI::valueWithFp(
        0, SmcKeyTypeFpe2, new FConstAc(kFanMaxRPM),
        SMC_KEY_ATTRIBUTE_READ));

    // F0Md: 2 = automatic (EC curve), 0 = forced full speed. Writing 0 spins
    // the fan up until it is set back to 2. This is the firmware's own
    // full-speed state, not a manual PWM - see setFanMode().
    VirtualSMCAPI::addKey(KeyF0Md, vsmcPlugin.data, VirtualSMCAPI::valueWithUint8(
        ASUS_FAN_CTRL_AUTO, new FMode(this, 0, &fanMode0),
        SMC_KEY_ATTRIBUTE_READ | SMC_KEY_ATTRIBUTE_WRITE | SMC_KEY_ATTRIBUTE_FUNCTION));

    FanTypeDescStruct desc;
    lilu_os_strncpy(desc.strFunction, "System Fan", DiagFunctionStrLen);

    VirtualSMCAPI::addKey(KeyF0ID, vsmcPlugin.data, VirtualSMCAPI::valueWithData(
        reinterpret_cast<const SMC_DATA *>(&desc), sizeof(desc), SmcKeyTypeFds, nullptr,
        SMC_KEY_ATTRIBUTE_CONST | SMC_KEY_ATTRIBUTE_READ));

    if (isFanPresent1) {
        VirtualSMCAPI::addKey(KeyF1Ac, vsmcPlugin.data, VirtualSMCAPI::valueWithFp(
            0, SmcKeyTypeFpe2, new F1Ac(&fanSpeed1),
            SMC_KEY_ATTRIBUTE_READ | SMC_KEY_ATTRIBUTE_FUNCTION));

        VirtualSMCAPI::addKey(KeyF1Mn, vsmcPlugin.data, VirtualSMCAPI::valueWithFp(
            0, SmcKeyTypeFpe2, new FConstAc(kFanMinRPM),
            SMC_KEY_ATTRIBUTE_READ));
        VirtualSMCAPI::addKey(KeyF1Mx, vsmcPlugin.data, VirtualSMCAPI::valueWithFp(
            0, SmcKeyTypeFpe2, new FConstAc(kFanMaxRPM),
            SMC_KEY_ATTRIBUTE_READ));

        VirtualSMCAPI::addKey(KeyF1Md, vsmcPlugin.data, VirtualSMCAPI::valueWithUint8(
            ASUS_FAN_CTRL_AUTO, new FMode(this, 1, &fanMode1),
            SMC_KEY_ATTRIBUTE_READ | SMC_KEY_ATTRIBUTE_WRITE | SMC_KEY_ATTRIBUTE_FUNCTION));

        FanTypeDescStruct desc1;
        lilu_os_strncpy(desc1.strFunction, "System Fan", DiagFunctionStrLen);

        VirtualSMCAPI::addKey(KeyF1ID, vsmcPlugin.data, VirtualSMCAPI::valueWithData(
            reinterpret_cast<const SMC_DATA *>(&desc1), sizeof(desc1), SmcKeyTypeFds, nullptr,
            SMC_KEY_ATTRIBUTE_CONST | SMC_KEY_ATTRIBUTE_READ));
    }

    // Thermal profile switch: 0 = balanced, 1 = performance, 2 = silent.
    // This selects which of the three firmware fan curves the EC runs, so it
    // is a single global key rather than one per fan. Named F0Tp
    // ("fan profile") to keep it distinct from F0Md, which is per-fan
    // full-speed/auto.
    if (isThermalPolicyPresent) {
        VirtualSMCAPI::addKey(KeyF0Tp, vsmcPlugin.data, VirtualSMCAPI::valueWithUint8(
            ASUS_THERMAL_POLICY_BALANCED, new FThermalProfile(this),
            SMC_KEY_ATTRIBUTE_READ | SMC_KEY_ATTRIBUTE_WRITE | SMC_KEY_ATTRIBUTE_FUNCTION));
    }

    if (isBatteryRSOCAvailable) {
        VirtualSMCAPI::addKey(KeyBDVT, vsmcPlugin.data, VirtualSMCAPI::valueWithFlag(
            false, new BDVT(this),
            SMC_KEY_ATTRIBUTE_READ | SMC_KEY_ATTRIBUTE_WRITE | SMC_KEY_ATTRIBUTE_ATOMIC));
    }

    qsort(const_cast<VirtualSMCKeyValue *>(vsmcPlugin.data.data()), vsmcPlugin.data.size(), sizeof(VirtualSMCKeyValue), VirtualSMCKeyValue::compare);
}

bool AsusSMC::vsmcNotificationHandler(void *sensors, void *refCon, IOService *vsmc, IONotifier *notifier) {
    if (sensors && vsmc) {
        DBGLOG("atk", "got vsmc notification");
        auto self = static_cast<AsusSMC *>(sensors);
        auto ret = vsmc->callPlatformFunction(VirtualSMCAPI::SubmitPlugin, true, sensors, &self->vsmcPlugin, nullptr, nullptr);
        if (ret == kIOReturnSuccess) {
            DBGLOG("atk", "Submitted plugin");

            self->workloop = self->getWorkLoop();
            self->poller = IOTimerEventSource::timerEventSource(self, [](OSObject *object, IOTimerEventSource *sender) {
                auto ls = OSDynamicCast(AsusSMC, object);
                if (ls) {
                    ls->refreshALS(true);
                    ls->refreshFan();
                    ls->followDisplayPower();
                }
            });

            if (!self->poller || !self->workloop) {
                SYSLOG("atk", "Failed to create poller or workloop");
                return false;
            }

            if (self->workloop->addEventSource(self->poller) != kIOReturnSuccess) {
                SYSLOG("atk", "Failed to add timer event source to workloop");
                return false;
            }

            if (self->poller->setTimeoutMS(SensorUpdateTimeoutMS) != kIOReturnSuccess) {
                SYSLOG("atk", "Failed to set timeout");
                return false;
            }

            return true;
        } else if (ret != kIOReturnUnsupported) {
            SYSLOG("atk", "Plugin submission failure %X", ret);
        } else {
            DBGLOG("atk", "Plugin submission to non vsmc");
        }
    } else {
        SYSLOG("atk", "Got null vsmc notification");
    }

    return false;
}

EXPORT extern "C" kern_return_t ADDPR(kern_start)(kmod_info_t *, void *) {
    // Report success but actually do not start and let I/O Kit unload us.
    // This works better and increases boot speed in some cases.
    PE_parse_boot_argn("liludelay", &ADDPR(debugPrintDelay), sizeof(ADDPR(debugPrintDelay)));
    ADDPR(debugEnabled) = checkKernelArgument("-vsmcdbg") || checkKernelArgument("-asussmcdbg");

    // Unconditional banner. This is a SYSLOG, not a DBGLOG, so it prints
    // even when debugging is off. Without it there is no way to tell from the
    // log which build actually got loaded, and no way to tell whether a
    // missing DBGLOG means "debug is off" or "the code never ran".
    SYSLOG("atk", "kern_start: debugEnabled=%d", ADDPR(debugEnabled) ? 1 : 0);

    if (!ADDPR(debugEnabled)) {
        SYSLOG("atk", "DBG output is OFF; add -asussmcdbg (or -vsmcdbg) to boot-args");
    }

    return KERN_SUCCESS;
}


EXPORT extern "C" kern_return_t ADDPR(kern_stop)(kmod_info_t *, void *) {
    // It is not safe to unload VirtualSMC plugins!
    return KERN_FAILURE;
}
