//
//  AsusSMC.cpp
//  AsusSMC
//
//  Copyright © 2018-2020 Le Bao Hiep. All rights reserved.
//

#include "AsusSMC.hpp"

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

    initVirtualKeyboard();

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

    registerService();

    return true;
}

void AsusSMC::stop(IOService *provider) {
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
                // The UI works in 0..16 levels; the firmware wants 0..level.
                uint8_t level = (uint8_t)(*((uint16_t *)argument) / 16);

                if (keyboardBacklightViaWMI) {
                    // DEVS 0x00050021 -> EC0.SLKB(level). There is no DSTS for
                    // this device, so nothing to read back.
                    int ret = wmi_evaluate_method(ASUS_WMI_METHODID_DEVS,
                                                  ASUS_WMI_DEVID_KEYBOARD_BACKLIGHT,
                                                  level);
                    if (ret == -1 || ret == (int)ASUS_WMI_UNSUPPORTED_METHOD) {
                        DBGLOG("kbd", "keyboard backlight DEVS failed");
                    }
                } else {
                    OSNumber *arg = OSNumber::withNumber(level, 16);
                    atkDevice->evaluateObject("SKBV", NULL, (OSObject **)&arg, 1);
                    arg->release();
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
 *  board implements it. We confirm by writing level 0 (backlight off) once;
 *  on a board without the device DEVS reports unsupported and nothing
 *  happens. Level 0 is the safe value to probe with, it cannot leave the
 *  backlight in a surprising state.
 */
bool AsusSMC::pci0Ec0HasKeyboardBacklight() {
    if (wmi_dev_is_present(ASUS_WMI_DEVID_KEYBOARD_BACKLIGHT)) {
        return true;
    }

    int ret = wmi_evaluate_method(ASUS_WMI_METHODID_DEVS,
                                  ASUS_WMI_DEVID_KEYBOARD_BACKLIGHT, 0);
    if (ret == -1 || ret == (int)ASUS_WMI_UNSUPPORTED_METHOD) {
        return false;
    }

    // DEVS on this chain answers 1 on success.
    return ret == 1;
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
    kbdDevice = new VirtualAppleKeyboard;

    if (!kbdDevice || !kbdDevice->init() || !kbdDevice->attach(this) || !kbdDevice->start(this)) {
        OSSafeReleaseNULL(kbdDevice);
        SYSLOG("vkbd", "Failed to init VirtualAppleKeyboard");
    }
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

void AsusSMC::readPanelBrightnessValue() {
    if (!findBacklightEntry()) {
        DBGLOG("atk", "GPU device not found");
        return;
    }

    IORegistryEntry *displayDeviceEntry = IORegistryEntry::fromPath(backlightEntry);

    if (displayDeviceEntry) {
        if (OSDictionary *ioDisplayParaDict = OSDynamicCast(OSDictionary, displayDeviceEntry->getProperty("IODisplayParameters"))) {
            if (OSDictionary *brightnessDict = OSDynamicCast(OSDictionary, ioDisplayParaDict->getObject("brightness"))) {
                if (OSNumber *brightnessValue = OSDynamicCast(OSNumber, brightnessDict->getObject("value"))) {
                    panelBrightnessLevel = brightnessValue->unsigned32BitValue() / 64;
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
    SMCKBrdBLightValue::lkb lkb;
    SMCKBrdBLightValue::lks lks;

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

    VirtualSMCAPI::addKey(KeyALV0, vsmcPlugin.data, VirtualSMCAPI::valueWithData(
        reinterpret_cast<const SMC_DATA *>(&emptyValue), sizeof(emptyValue), SmcKeyTypeAlv, new SMCALSValue(&currentLux, &forceBits),
        SMC_KEY_ATTRIBUTE_READ | SMC_KEY_ATTRIBUTE_WRITE | SMC_KEY_ATTRIBUTE_FUNCTION));

    VirtualSMCAPI::addKey(KeyALV1, vsmcPlugin.data, VirtualSMCAPI::valueWithData(
        reinterpret_cast<const SMC_DATA *>(&emptyValue), sizeof(emptyValue), SmcKeyTypeAlv, nullptr,
        SMC_KEY_ATTRIBUTE_READ | SMC_KEY_ATTRIBUTE_WRITE | SMC_KEY_ATTRIBUTE_FUNCTION));

    VirtualSMCAPI::addKey(KeyLKSB, vsmcPlugin.data, VirtualSMCAPI::valueWithData(
        reinterpret_cast<const SMC_DATA *>(&lkb), sizeof(lkb), SmcKeyTypeLkb, new SMCKBrdBLightValue(this),
        SMC_KEY_ATTRIBUTE_READ | SMC_KEY_ATTRIBUTE_WRITE | SMC_KEY_ATTRIBUTE_FUNCTION));

    VirtualSMCAPI::addKey(KeyLKSS, vsmcPlugin.data, VirtualSMCAPI::valueWithData(
        reinterpret_cast<const SMC_DATA *>(&lks), sizeof(lks), SmcKeyTypeLks, nullptr,
        SMC_KEY_ATTRIBUTE_READ | SMC_KEY_ATTRIBUTE_WRITE | SMC_KEY_ATTRIBUTE_FUNCTION));

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
    return KERN_SUCCESS;
}

EXPORT extern "C" kern_return_t ADDPR(kern_stop)(kmod_info_t *, void *) {
    // It is not safe to unload VirtualSMC plugins!
    return KERN_FAILURE;
}
