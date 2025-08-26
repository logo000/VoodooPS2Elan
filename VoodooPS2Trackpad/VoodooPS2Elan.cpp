/*
* Elan PS2 touchpad integration
*
* Mostly contains code ported from Linux
* https://github.com/torvalds/linux/blob/master/drivers/input/mouse/elantech.c
*
* Created by Bartosz Korczyński (@bandysc), Hiep Bao Le (@hieplpvip)
* Special thanks to Kishor Prins (@kprinssu), EMlyDinEsHMG and whole VoodooInput team
*/

// generally one cannot IOLog from interrupt context, it eventually leads to kernel panic
// but it is useful sometimes
#if 1
#define INTERRUPT_LOG(args...)  do { IOLog(args); } while (0)
#else
#define INTERRUPT_LOG(args...)  do { } while (0)
#endif

// ETD0180 firmware version macro for cleaner code
#define IS_ETD0180() (info.fw_version == 0x381f17)

#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/hidsystem/IOHIDParameter.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/usb/IOUSBHostFamily.h>
#include <IOKit/usb/IOUSBHostHIDDevice.h>
#include <IOKit/bluetooth/BluetoothAssignedNumbers.h>
#include <mach/mach_time.h>
#include "VoodooPS2Controller.h"
#include "VoodooPS2Elan.h"
#include "VoodooInputMultitouch/VoodooInputTransducer.h"
#include "VoodooInputMultitouch/VoodooInputMessages.h"

// =============================================================================
// ApplePS2Elan Class Implementation
//

OSDefineMetaClassAndStructors(ApplePS2Elan, IOService);

bool ApplePS2Elan::init(OSDictionary *dict) {
    // Initialize this object's minimal state. This is invoked right after this
    // object is instantiated.

    if (!super::init(dict)) {
        return false;
    }

    // announce version
    extern kmod_info_t kmod_info;
    DEBUG_LOG("VoodooPS2Elan: Version %s starting on OS X Darwin %d.%d.\n", kmod_info.version, version_major, version_minor);

    return true;
}

void ApplePS2Elan::injectVersionDependentProperties(OSDictionary *config) {
    // inject properties specific to the version of Darwin that is runnning...
    char buf[32];
    OSDictionary *dict = NULL;
    do {
        // check for "Darwin major.minor"
        snprintf(buf, sizeof(buf), "Darwin %d.%d", version_major, version_minor);
        if ((dict = OSDynamicCast(OSDictionary, config->getObject(buf)))) {
            break;
        }

        // check for "Darwin major.x"
        snprintf(buf, sizeof(buf), "Darwin %d.x", version_major);
        if ((dict = OSDynamicCast(OSDictionary, config->getObject(buf)))) {
            break;
        }

        // check for "Darwin 16+" (this is what is used currently, other formats are for future)
        if (version_major >= 16 && (dict = OSDynamicCast(OSDictionary, config->getObject("Darwin 16+")))) {
            break;
        }
    } while (0);

    if (dict) {
        // found version specific properties above, inject...
        if (OSCollectionIterator *iter = OSCollectionIterator::withCollection(dict)) {
            // Note: OSDictionary always contains OSSymbol*
            while (const OSSymbol *key = static_cast<const OSSymbol*>(iter->getNextObject())) {
                if (OSObject *value = dict->getObject(key)) {
                    setProperty(key, value);
                }
            }
            iter->release();
        }
    }
}

ApplePS2Elan *ApplePS2Elan::probe(IOService *provider, SInt32 *score) {
    DEBUG_LOG("ApplePS2Elan::probe entered...\n");

    // The driver has been instructed to verify the presence of the actual
    // hardware we represent. We are guaranteed by the controller that the
    // mouse clock is enabled and the mouse itself is disabled (thus it
    // won't send any asynchronous mouse data that may mess up the
    // responses expected by the commands we send it).

    if (!super::probe(provider, score)) {
        DEBUG_LOG("ApplePS2Elan: super::probe() failed\n");
        return 0;
    }

    _device = (ApplePS2MouseDevice*)provider;

    // find config specific to Platform Profile
    OSDictionary *list = OSDynamicCast(OSDictionary, getProperty(kPlatformProfile));
    OSDictionary *config = _device->getController()->makeConfigurationNode(list, "Elantech TouchPad");
    if (config) {
        // if DisableDevice is Yes, then do not load at all...
        OSBoolean *disable = OSDynamicCast(OSBoolean, config->getObject(kDisableDevice));
        if (disable && disable->isTrue()) {
            config->release();
            _device = 0;
            return 0;
        }
#ifdef DEBUG
        // save configuration for later/diagnostics...
        setProperty(kMergedConfiguration, config);
#endif

        // load settings specific to Platform Profile
        setParamPropertiesGated(config);
        injectVersionDependentProperties(config);
        OSSafeReleaseNULL(config);
    }

    resetMouse();

    DEBUG_LOG("VoodooPS2Elan: Detecting Elantech device\n");
    DEBUG_LOG("VoodooPS2Elan: send magic knock to the device.\n");
    // send magic knock to the device
    if (elantechDetect()) {
        DEBUG_LOG("VoodooPS2Elan: elantechDetect() failed - not an Elantech device\n");
        DEBUG_LOG("VoodooPS2Elan: elan touchpad not detected\n");
        return NULL;
    }
    DEBUG_LOG("VoodooPS2Elan: elantechDetect() SUCCESS - Elantech device confirmed\n");

    resetMouse();

    DEBUG_LOG("VoodooPS2Elan: Querying device info\n");
    if (elantechQueryInfo()) {
        DEBUG_LOG("VoodooPS2Elan: elantechQueryInfo() FAILED\n");
        DEBUG_LOG("VoodooPS2Elan: query info failed\n");
        return NULL;
    }
    DEBUG_LOG("VoodooPS2Elan: elantechQueryInfo() SUCCESS, fw=0x%06x\n", info.fw_version);

    DEBUG_LOG("VoodooPS2Elan: capabilities: %x %x %x\n", info.capabilities[0], info.capabilities[1], info.capabilities[2]);
    DEBUG_LOG("VoodooPS2Elan: samples: %x %x %x\n", info.capabilities[0], info.capabilities[1], info.capabilities[2]);
    DEBUG_LOG("VoodooPS2Elan: hw_version: %x\n", info.hw_version);
    DEBUG_LOG("VoodooPS2Elan: fw_version: %x\n", info.fw_version);
    IOLog("VoodooPS2Elan: FIRMWARE_VERSION=0x%06x IS_ETD0180=%s\n", info.fw_version, IS_ETD0180() ? "YES" : "NO");
    DEBUG_LOG("VoodooPS2Elan: x_min: %d\n", info.x_min);
    DEBUG_LOG("VoodooPS2Elan: y_min: %d\n", info.y_min);
    DEBUG_LOG("VoodooPS2Elan: x_max: %d\n", info.x_max);
    DEBUG_LOG("VoodooPS2Elan: y_max: %d\n", info.y_max);
    DEBUG_LOG("VoodooPS2Elan: x_res: %d\n", info.x_res);
    DEBUG_LOG("VoodooPS2Elan: y_res: %d\n", info.y_res);
    DEBUG_LOG("VoodooPS2Elan: x_traces: %d\n", info.x_traces);
    DEBUG_LOG("VoodooPS2Elan: y_traces: %d\n", info.y_traces);
    DEBUG_LOG("VoodooPS2Elan: width: %d\n", info.width);
    DEBUG_LOG("VoodooPS2Elan: bus: %d\n", info.bus);
    DEBUG_LOG("VoodooPS2Elan: paritycheck: %d\n", info.paritycheck);
    DEBUG_LOG("VoodooPS2Elan: jumpy_cursor: %d\n", info.jumpy_cursor);
    DEBUG_LOG("VoodooPS2Elan: reports_pressure: %d\n", info.reports_pressure);
    DEBUG_LOG("VoodooPS2Elan: crc_enabled: %d\n", info.crc_enabled);
    DEBUG_LOG("VoodooPS2Elan: set_hw_resolution: %d\n", info.set_hw_resolution);
    DEBUG_LOG("VoodooPS2Elan: has_trackpoint: %d\n", info.has_trackpoint);
    DEBUG_LOG("VoodooPS2Elan: has_middle_button: %d\n", info.has_middle_button);

    DEBUG_LOG("VoodooPS2Elan: elan touchpad detected. Probing finished.\n");
    DEBUG_LOG("VoodooPS2Elan: probe() FINISHED SUCCESSFULLY - ApplePS2Elan will be used\n");

    _device = nullptr;

    return this;
}

bool ApplePS2Elan::handleOpen(IOService *forClient, IOOptionBits options, void *arg) {
    if (forClient && forClient->getProperty(VOODOO_INPUT_IDENTIFIER)) {
        voodooInputInstance = forClient;
        voodooInputInstance->retain();

        return true;
    }

    return false;
}

bool ApplePS2Elan::handleIsOpen(const IOService *forClient) const {
    if (forClient == nullptr) {
        return voodooInputInstance != nullptr;
    } else {
        return voodooInputInstance == forClient;
    }
}

void ApplePS2Elan::handleClose(IOService *forClient, IOOptionBits options) {
    if (forClient == voodooInputInstance) {
        OSSafeReleaseNULL(voodooInputInstance);
    }
}

bool ApplePS2Elan::start(IOService *provider) {
    // The driver has been instructed to start. This is called after a
    // successful probe and match.

    if (!super::start(provider)) {
        return false;
    }

    // Maintain a pointer to and retain the provider object.
    _device = (ApplePS2MouseDevice *)provider;
    _device->retain();

    // Announce hardware properties.
    char buf[128];
    snprintf(buf, sizeof(buf), "Elan v %d, fw: %x, bus: %d", info.hw_version, info.fw_version, info.bus);
    setProperty("RM,TrackpadInfo", buf);

#ifdef DEBUG
    if (info.bus == ETP_BUS_PS2_ONLY) {
        setProperty("Bus", "ETP_BUS_PS2_ONLY");
    } else if (info.bus == ETP_BUS_SMB_ALERT_ONLY) {
        setProperty("Bus", "ETP_BUS_SMB_ALERT_ONLY");
    } else if (info.bus == ETP_BUS_SMB_HST_NTFY_ONLY) {
        setProperty("Bus", "ETP_BUS_SMB_HST_NTFY_ONLY");
    } else if (info.bus == ETP_BUS_PS2_SMB_ALERT) {
        setProperty("Bus", "ETP_BUS_PS2_SMB_ALERT");
    } else if (info.bus == ETP_BUS_PS2_SMB_HST_NTFY) {
        setProperty("Bus", "ETP_BUS_PS2_SMB_HST_NTFY");
    }

    if (info.bus == ETP_BUS_SMB_HST_NTFY_ONLY ||
        info.bus == ETP_BUS_PS2_SMB_HST_NTFY ||
        ETP_NEW_IC_SMBUS_HOST_NOTIFY(info.fw_version)) {
        setProperty("SMBus NOTE", "It looks like your touchpad is supported by VoodooSMBus kext, which gives better multitouch experience. We recommend you to try it.");
    } else if (info.bus == ETP_BUS_PS2_ONLY) {
        setProperty("SMBus NOTE", "It looks like your touchpad does not support SMBus protocol.");
    }
#endif

    // Setup workloop with command gate for thread syncronization...
    IOWorkLoop *pWorkLoop = getWorkLoop();
    IOLog("VoodooPS2Elan: WorkLoop = %p\n", pWorkLoop);
    _cmdGate = IOCommandGate::commandGate(this);
    IOLog("VoodooPS2Elan: CommandGate = %p\n", _cmdGate);
    if (!pWorkLoop || !_cmdGate) {
        IOLog("VoodooPS2Elan: FAILED - WorkLoop=%p, CommandGate=%p\n", pWorkLoop, _cmdGate);
        OSSafeReleaseNULL(_device);
        return false;
    }
    IOLog("VoodooPS2Elan: WorkLoop/CommandGate setup SUCCESS, proceeding to elantechSetupPS2\n");

    // Lock the controller during initialization
    _device->lock();

    attachedHIDPointerDevices = OSSet::withCapacity(1);
    registerHIDPointerNotifications();

    pWorkLoop->addEventSource(_cmdGate);
    
    // Setup button timer event source for middle button simulation
    _buttonTimer = IOTimerEventSource::timerEventSource(this, OSMemberFunctionCast(IOTimerEventSource::Action, this, &ApplePS2Elan::onButtonTimer));
    if (_buttonTimer) {
        pWorkLoop->addEventSource(_buttonTimer);
        IOLog("VoodooPS2Elan: Button timer setup SUCCESS\n");
    } else {
        IOLog("VoodooPS2Elan: FAILED to create button timer\n");
    }

    elantechSetupPS2();

    // Install our driver's interrupt handler, for asynchronous data delivery.
    _device->installInterruptAction(this,
                                    OSMemberFunctionCast(PS2InterruptAction, this, &ApplePS2Elan::interruptOccurred),
                                    OSMemberFunctionCast(PS2PacketAction, this, &ApplePS2Elan::packetReady));
    _interruptHandlerInstalled = true;

    // Enable the touchpad
    setTouchPadEnable(true);

    // Now it is safe to allow other threads
    _device->unlock();

    // Install our power control handler
    _device->installPowerControlAction(this, OSMemberFunctionCast(PS2PowerControlAction, this, &ApplePS2Elan::setDevicePowerState));
    _powerControlHandlerInstalled = true;

    // Request message registration for keyboard to trackpad communication
    //setProperty(kDeliverNotifications, true);

    return true;
}

void ApplePS2Elan::stop(IOService *provider) {
    DEBUG_LOG("%s: stop called\n", getName());

    // The driver has been instructed to stop. Note that we must break all
    // connections to other service objects now (ie. no registered actions,
    // no pointers and retains to objects, etc), if any.

    assert(_device == provider);

    unregisterHIDPointerNotifications();
    OSSafeReleaseNULL(attachedHIDPointerDevices);

    // Disable the touchpad
    setTouchPadEnable(false);

    // Release command gate and button timer
    IOWorkLoop *pWorkLoop = getWorkLoop();
    if (pWorkLoop) {
        if (_cmdGate) {
            pWorkLoop->removeEventSource(_cmdGate);
            OSSafeReleaseNULL(_cmdGate);
        }
        if (_buttonTimer) {
            pWorkLoop->removeEventSource(_buttonTimer);
            OSSafeReleaseNULL(_buttonTimer);
        }
    }

    // Uninstall the interrupt handler
    if (_interruptHandlerInstalled) {
        _device->uninstallInterruptAction();
        _interruptHandlerInstalled = false;
    }

    // Uninstall the power control handler
    if (_powerControlHandlerInstalled) {
        _device->uninstallPowerControlAction();
        _powerControlHandlerInstalled = false;
    }

    // Release the pointer to the provider object.
    OSSafeReleaseNULL(_device);

    super::stop(provider);
}

void ApplePS2Elan::setParamPropertiesGated(OSDictionary *config) {
    if (NULL == config) {
        return;
    }

    const struct {const char *name; int *var;} int32vars[] = {
        {"WakeDelay",                          &wakedelay},
        {"TrackpointDeadzone",                 &_trackpointDeadzone},
        {"TrackpointMultiplierX",              &_trackpointMultiplierX},
        {"TrackpointMultiplierY",              &_trackpointMultiplierY},
        {"TrackpointDividerX",                 &_trackpointDividerX},
        {"TrackpointDividerY",                 &_trackpointDividerY},
        {"TrackpointScrollMultiplierX",        &_trackpointScrollMultiplierX},
        {"TrackpointScrollMultiplierY",        &_trackpointScrollMultiplierY},
        {"TrackpointScrollDividerY",           &_trackpointScrollDividerX},
        {"TrackpointScrollDividerY",           &_trackpointScrollDividerY},
        {"MouseResolution",                    &_mouseResolution},
        {"MouseSampleRate",                    &_mouseSampleRate},
        {"ForceTouchMode",                     (int*)&_forceTouchMode},
        {"FakeMiddleButton",                   &_fakemiddlebutton},
    };

    const struct {const char *name; uint64_t *var;} int64vars[] = {
        {"QuietTimeAfterTyping",               &maxaftertyping},
        {"MiddleClickTime",                    &_maxmiddleclicktime},
    };

    const struct {const char *name; bool *var;} boolvars[] = {
        {"ProcessUSBMouseStopsTrackpad",       &_processusbmouse},
        {"ProcessBluetoothMouseStopsTrackpad", &_processbluetoothmouse},
        {"SetHwResolution",                    &_set_hw_resolution},
    };

    const struct {const char *name; bool *var;} lowbitvars[] = {
        {"USBMouseStopsTrackpad",              &usb_mouse_stops_trackpad},
    };

    OSBoolean *bl;
    OSNumber *num;

    // highrate?
    if ((bl = OSDynamicCast(OSBoolean, config->getObject("UseHighRate")))) {
        setProperty("UseHighRate", bl->isTrue());
    }

    // 32-bit config items
    for (int i = 0; i < countof(int32vars); i++) {
        if ((num = OSDynamicCast(OSNumber, config->getObject(int32vars[i].name)))) {
            *int32vars[i].var = num->unsigned32BitValue();
            setProperty(int32vars[i].name, *int32vars[i].var, 32);
        }
    }

    // 64-bit config items
    for (int i = 0; i < countof(int64vars); i++) {
        if ((num = OSDynamicCast(OSNumber, config->getObject(int64vars[i].name)))) {
            *int64vars[i].var = num->unsigned64BitValue();
            setProperty(int64vars[i].name, *int64vars[i].var, 64);
        }
    }

    // boolean config items
    for (int i = 0; i < countof(boolvars); i++) {
        if ((bl = OSDynamicCast(OSBoolean, config->getObject(boolvars[i].name)))) {
            *boolvars[i].var = bl->isTrue();
            setProperty(boolvars[i].name, *boolvars[i].var ? kOSBooleanTrue : kOSBooleanFalse);
        }
    }

    // lowbit config items
    for (int i = 0; i < countof(lowbitvars); i++) {
        if ((num = OSDynamicCast(OSNumber, config->getObject(lowbitvars[i].name)))) {
            *lowbitvars[i].var = (num->unsigned32BitValue() & 0x1) ? true : false;
            setProperty(lowbitvars[i].name, *lowbitvars[i].var ? 1 : 0, 32);
        } else if ((bl = OSDynamicCast(OSBoolean, config->getObject(lowbitvars[i].name)))) {
            // REVIEW: are these items ever carried in a boolean?
            *lowbitvars[i].var = bl->isTrue();
            setProperty(lowbitvars[i].name, *lowbitvars[i].var ? kOSBooleanTrue : kOSBooleanFalse);
        }
    }

    // disable trackpad when USB mouse is plugged in and this functionality is requested
    if (attachedHIDPointerDevices && attachedHIDPointerDevices->getCount() > 0) {
        ignoreall = usb_mouse_stops_trackpad;
    }
    
    setTrackpointProperties();
}

IOReturn ApplePS2Elan::setProperties(OSObject *props) {
    OSDictionary *dict = OSDynamicCast(OSDictionary, props);
    if (dict && _cmdGate) {
        // synchronize through workloop...
        _cmdGate->runAction(OSMemberFunctionCast(IOCommandGate::Action, this, &ApplePS2Elan::setParamPropertiesGated), dict);
    }

    return super::setProperties(props);
}

void ApplePS2Elan::setTrackpointProperties()
{
    // ELAN Touchpads are pure touchpads, not trackpoint+touchpad hybrids!
    // Trackpoint properties cause VoodooInput to create unwanted TrackpointDevice.
    // For ELAN touchpads: NO trackpoint properties = NO TrackpointDevice = ONLY multitouch trackpad
    IOLog("VoodooPS2Elan: setTrackpointProperties() disabled - ELAN is pure touchpad, not trackpoint hybrid\n");
    return;
    
}

IOReturn ApplePS2Elan::message(UInt32 type, IOService* provider, void* argument) {
    // Here is where we receive messages from the keyboard driver
    //
    // This allows for the keyboard driver to enable/disable the trackpad
    // when a certain keycode is pressed.
    //
    // It also allows the trackpad driver to learn the last time a key
    // has been pressed, so it can implement various "ignore trackpad
    // input while typing" options.
    switch (type) {
        case kPS2M_getDisableTouchpad:
        {
            bool* pResult = (bool*)argument;
            *pResult = !ignoreall;
            break;
        }

        case kPS2M_setDisableTouchpad:
        {
            bool enable = *((bool*)argument);
            ignoreall = !enable;
            break;
        }

        case kPS2M_resetTouchpad:
        {
            int *reqCode = (int *)argument;
            DEBUG_LOG("VoodooPS2Elan::kPS2M_resetTouchpad reqCode: %d\n", *reqCode);
            if (*reqCode == 1) {
                setTouchPadEnable(false);
                IOSleep(wakedelay);

                ignoreall = false;
                _packetByteCount = 0;
                _ringBuffer.reset();

                resetMouse();
                elantechSetupPS2();
                setTouchPadEnable(true);
            }
            break;
        }

        case kPS2M_notifyKeyTime:
        {
            // just remember last time key pressed... this can be used in
            // interrupt handler to detect unintended input while typing
            keytime = *((uint64_t*)argument);
            break;
        }
    }

    return kIOReturnSuccess;
}

void ApplePS2Elan::setDevicePowerState(UInt32 whatToDo) {
    switch (whatToDo) {
        case kPS2C_DisableDevice:
            // Disable the touchpad
            setTouchPadEnable(false);
            break;

        case kPS2C_EnableDevice:
            // Must not issue any commands before the device has
            // completed its power-on self-test and calibration
            IOSleep(wakedelay);

            // Clear packet buffer pointer to avoid issues caused by stale packet fragments
            _packetByteCount = 0;
            _ringBuffer.reset();

            // Reset and enable the touchpad
            resetMouse();
            elantechSetupPS2();
            setTouchPadEnable(true);
            break;
    }
}

void ApplePS2Elan::registerHIDPointerNotifications() {
    IOServiceMatchingNotificationHandler notificationHandler = OSMemberFunctionCast(IOServiceMatchingNotificationHandler, this, &ApplePS2Elan::notificationHIDAttachedHandler);

    // Determine if we should listen for USB mouse attach events as per configuration
    if (_processusbmouse) {
        // USB mouse HID description as per USB spec: http://www.usb.org/developers/hidpage/HID1_11.pdf
        OSDictionary *matchingDictionary = serviceMatching("IOUSBInterface");

        propertyMatching(OSSymbol::withCString(kUSBHostMatchingPropertyInterfaceClass), OSNumber::withNumber(kUSBHIDInterfaceClass, 8), matchingDictionary);
        propertyMatching(OSSymbol::withCString(kUSBHostMatchingPropertyInterfaceSubClass), OSNumber::withNumber(kUSBHIDBootInterfaceSubClass, 8), matchingDictionary);
        propertyMatching(OSSymbol::withCString(kUSBHostMatchingPropertyInterfaceProtocol), OSNumber::withNumber(kHIDMouseInterfaceProtocol, 8), matchingDictionary);

        // Register for future services
        usb_hid_publish_notify = addMatchingNotification(gIOFirstPublishNotification, matchingDictionary, notificationHandler, this, NULL, 10000);
        usb_hid_terminate_notify = addMatchingNotification(gIOTerminatedNotification, matchingDictionary, notificationHandler, this, NULL, 10000);
        OSSafeReleaseNULL(matchingDictionary);
    }

    // Determine if we should listen for bluetooth mouse attach events as per configuration
    if (_processbluetoothmouse) {
        // Bluetooth HID devices
        OSDictionary *matchingDictionary = serviceMatching("IOBluetoothHIDDriver");
        propertyMatching(OSSymbol::withCString(kIOHIDVirtualHIDevice), kOSBooleanFalse, matchingDictionary);

        // Register for future services
        bluetooth_hid_publish_notify = addMatchingNotification(gIOFirstPublishNotification, matchingDictionary, notificationHandler, this, NULL, 10000);
        bluetooth_hid_terminate_notify = addMatchingNotification(gIOTerminatedNotification, matchingDictionary, notificationHandler, this, NULL, 10000);
        OSSafeReleaseNULL(matchingDictionary);
    }
}

void ApplePS2Elan::unregisterHIDPointerNotifications() {
    // Free device matching notifiers
    // remove() releases them

    if (usb_hid_publish_notify) {
        usb_hid_publish_notify->remove();
    }

    if (usb_hid_terminate_notify) {
        usb_hid_terminate_notify->remove();
    }

    if (bluetooth_hid_publish_notify) {
        bluetooth_hid_publish_notify->remove();
    }

    if (bluetooth_hid_terminate_notify) {
        bluetooth_hid_terminate_notify->remove();
    }

    attachedHIDPointerDevices->flushCollection();
}

void ApplePS2Elan::notificationHIDAttachedHandlerGated(IOService *newService, IONotifier *notifier) {
    char path[256];
    int len = 255;
    memset(path, 0, len);
    newService->getPath(path, &len, gIOServicePlane);

    if (notifier == usb_hid_publish_notify) {
        attachedHIDPointerDevices->setObject(newService);
        DEBUG_LOG("%s: USB pointer HID device published: %s, # devices: %d\n", getName(), path, attachedHIDPointerDevices->getCount());
    }

    if (notifier == usb_hid_terminate_notify) {
        attachedHIDPointerDevices->removeObject(newService);
        DEBUG_LOG("%s: USB pointer HID device terminated: %s, # devices: %d\n", getName(), path, attachedHIDPointerDevices->getCount());
    }

    if (notifier == bluetooth_hid_publish_notify) {
        // Filter on specific CoD (Class of Device) bluetooth devices only
        OSNumber *propDeviceClass = OSDynamicCast(OSNumber, newService->getProperty("ClassOfDevice"));

        if (propDeviceClass != NULL) {
            UInt32 classOfDevice = propDeviceClass->unsigned32BitValue();

            UInt32 deviceClassMajor = (classOfDevice & 0x1F00) >> 8;
            UInt32 deviceClassMinor = (classOfDevice & 0xFF) >> 2;

            if (deviceClassMajor == kBluetoothDeviceClassMajorPeripheral) { // Bluetooth peripheral devices
                UInt32 deviceClassMinor1 = (deviceClassMinor) & 0x30;
                UInt32 deviceClassMinor2 = (deviceClassMinor) & 0x0F;

                if (deviceClassMinor1 == kBluetoothDeviceClassMinorPeripheral1Pointing || // Seperate pointing device
                    deviceClassMinor1 == kBluetoothDeviceClassMinorPeripheral1Combo) // Combo bluetooth keyboard/touchpad
                {
                    if (deviceClassMinor2 == kBluetoothDeviceClassMinorPeripheral2Unclassified || // Mouse
                        deviceClassMinor2 == kBluetoothDeviceClassMinorPeripheral2DigitizerTablet || // Magic Touchpad
                        deviceClassMinor2 == kBluetoothDeviceClassMinorPeripheral2DigitalPen) // Wacom Tablet
                    {
                        attachedHIDPointerDevices->setObject(newService);
                        DEBUG_LOG("%s: Bluetooth pointer HID device published: %s, # devices: %d\n", getName(), path, attachedHIDPointerDevices->getCount());
                    }
                }
            }
        }
    }

    if (notifier == bluetooth_hid_terminate_notify) {
        attachedHIDPointerDevices->removeObject(newService);
        DEBUG_LOG("%s: Bluetooth pointer HID device terminated: %s, # devices: %d\n", getName(), path, attachedHIDPointerDevices->getCount());
    }

    if (notifier == usb_hid_publish_notify || notifier == bluetooth_hid_publish_notify) {
        if (usb_mouse_stops_trackpad && attachedHIDPointerDevices->getCount() > 0) {
            // One or more USB or Bluetooth pointer devices attached, disable trackpad
            ignoreall = true;
        }
    }

    if (notifier == usb_hid_terminate_notify || notifier == bluetooth_hid_terminate_notify) {
        if (usb_mouse_stops_trackpad && attachedHIDPointerDevices->getCount() == 0) {
            // No USB or bluetooth pointer devices attached, re-enable trackpad
            ignoreall = false;
        }
    }
}

bool ApplePS2Elan::notificationHIDAttachedHandler(void *refCon, IOService *newService, IONotifier *notifier) {
    if (_cmdGate) {
        _cmdGate->runAction(OSMemberFunctionCast(IOCommandGate::Action, this, &ApplePS2Elan::notificationHIDAttachedHandlerGated), newService, notifier);
    }

    return true;
}

// elantech.c port

template<int I>
int ApplePS2Elan::ps2_command(UInt8 *params, unsigned int command) {
    TPS2Request<1 + I> request;
    request.commands[0].command = kPS2C_SendCommandAndCompareAck;
    request.commands[0].inOrOut = command;
    for (int i = 0; i < I; i++) {
        request.commands[1 + i].command = kPS2C_ReadDataPort;
    }

    request.commandsCount = 1 + I;
    assert(request.commandsCount <= countof(request.commands));
    _device->submitRequestAndBlock(&request);

    for (int i = 0; i < I; i++) {
        params[i] = request.commands[i + 1].inOrOut;
    }

    return request.commandsCount != 1 + I;
}

/*
 * A retrying version of ps2_command
 */
template<int I>
int ApplePS2Elan::elantech_ps2_command(unsigned char *param, int command) {
    int rc;
    int tries = ETP_PS2_COMMAND_TRIES;

    do {
        rc = ps2_command<I>(param, command);
        if (rc == 0) {
            break;
        }
        tries--;
        DEBUG_LOG("VoodooPS2Elan: retrying ps2 command 0x%02x (%d).\n", command, tries);
        IOSleep(ETP_PS2_COMMAND_DELAY);
    } while (tries > 0);

    if (rc) {
        DEBUG_LOG("VoodooPS2Elan: ps2 command 0x%02x failed.\n", command);
    }

    return rc;
}

/*
 * ps2_sliced_command() sends an extended PS/2 command to the mouse
 * using sliced syntax, understood by advanced devices, such as Logitech
 * or Synaptics touchpads. The command is encoded as:
 * 0xE6 0xE8 rr 0xE8 ss 0xE8 tt 0xE8 uu where (rr*64)+(ss*16)+(tt*4)+uu
 * is the command.
 */
int ApplePS2Elan::ps2_sliced_command(UInt8 command) {
    int j = 0;

    TPS2Request<> request;
    request.commands[j].command = kPS2C_SendCommandAndCompareAck;
    request.commands[j++].inOrOut = kDP_SetMouseScaling1To1;

    for (int i = 6; i >= 0; i -= 2) {
        UInt8 d = (command >> i) & 3;
        request.commands[j].command = kPS2C_SendCommandAndCompareAck;
        request.commands[j++].inOrOut = kDP_SetMouseResolution;

        request.commands[j].command = kPS2C_SendCommandAndCompareAck;
        request.commands[j++].inOrOut = d;
    }

    request.commandsCount = j;
    _device->submitRequestAndBlock(&request);

    return request.commandsCount != j;
}

/*
 * Send a Synaptics style sliced query command
 */
template<int I>
int ApplePS2Elan::synaptics_send_cmd(unsigned char c, unsigned char *param) {
    if (ps2_sliced_command(c) || ps2_command<I>(param, kDP_GetMouseInformation)) {
        DEBUG_LOG("VoodooPS2Elan: query 0x%02x failed.\n", c);
        return -1;
    }

    return 0;
}

/*
 * V3 and later support this fast command
 */
template<int I>
int ApplePS2Elan::elantech_send_cmd(unsigned char c, unsigned char *param) {
    if (ps2_command<0>(NULL, ETP_PS2_CUSTOM_COMMAND) ||
        ps2_command<0>(NULL, c) ||
        ps2_command<I>(param, kDP_GetMouseInformation)) {
        DEBUG_LOG("VoodooPS2Elan: query 0x%02x failed.\n", c);
        return -1;
    }

    return 0;
}

template<int I>
int ApplePS2Elan::send_cmd(unsigned char c, unsigned char *param) {
    if (info.hw_version >= 3) {
        return elantech_send_cmd<I>(c, param);
    } else {
        return synaptics_send_cmd<I>(c, param);
    }
}

bool ApplePS2Elan::elantech_is_signature_valid(const unsigned char *param) {
    static const unsigned char rates[] = { 200, 100, 80, 60, 40, 20, 10 };

    if (param[0] == 0) {
        return false;
    }

    if (param[1] == 0) {
        return true;
    }

    // Some hw_version >= 4 models have a revision higher then 20.
    // Meaning that param[2] may be 10 or 20, skip the rates check for these.
    if ((param[0] & 0x0f) >= 0x06 && (param[1] & 0xaf) == 0x0f && param[2] < 40) {
        return true;
    }

    for (int i = 0; i < sizeof(rates) / sizeof(*rates); i++) {
        if (param[2] == rates[i]) {
            return false;
        }
    }

    return true;
}

/*
 * (value from firmware) * 10 + 790 = dpi
 * we also have to convert dpi to dots/mm (*10/254 to avoid floating point)
 */
unsigned int ApplePS2Elan::elantech_convert_res(unsigned int val) {
    return (val * 10 + 790) * 10 / 254;
}

int ApplePS2Elan::elantech_get_resolution_v4(unsigned int *x_res, unsigned int *y_res, unsigned int *bus) {
    unsigned char param[3];

    if (elantech_send_cmd<3>(ETP_RESOLUTION_QUERY, param)) {
        return -1;
    }

    *x_res = elantech_convert_res(param[1] & 0x0f);
    *y_res = elantech_convert_res((param[1] & 0xf0) >> 4);
    *bus = param[2];

    return 0;
}

/*
 * Use magic knock to detect Elantech touchpad
 */
int ApplePS2Elan::elantechDetect() {
    unsigned char param[3];

    if (ps2_command<0>(NULL, kDP_SetDefaults) ||
        ps2_command<0>(NULL, kDP_SetDefaultsAndDisable) ||
        ps2_command<0>(NULL, kDP_SetMouseScaling1To1) ||
        ps2_command<0>(NULL, kDP_SetMouseScaling1To1) ||
        ps2_command<0>(NULL, kDP_SetMouseScaling1To1) ||
        ps2_command<3>(param, kDP_GetMouseInformation)) {
        DEBUG_LOG("VoodooPS2Elan: sending Elantech magic knock failed.\n");
        return -1;
    }

    // Report this in case there are Elantech models that use a different
    // set of magic numbers
    if (param[0] != 0x3c || param[1] != 0x03 || (param[2] != 0xc8 && param[2] != 0x00)) {
        DEBUG_LOG("VoodooPS2Elan: unexpected magic knock result 0x%02x, 0x%02x, 0x%02x.\n", param[0], param[1], param[2]);
        return -1;
    }

    // Query touchpad's firmware version and see if it reports known
    // value to avoid mis-detection. Logitech mice are known to respond
    // to Elantech magic knock and there might be more.
    if (synaptics_send_cmd<3>(ETP_FW_VERSION_QUERY, param)) {
        DEBUG_LOG("VoodooPS2Elan: failed to query firmware version.\n");
        return -1;
    }

    DEBUG_LOG("VoodooPS2Elan: Elantech version query result 0x%02x, 0x%02x, 0x%02x.\n", param[0], param[1], param[2]);

    if (!elantech_is_signature_valid(param)) {
        DEBUG_LOG("VoodooPS2Elan: Probably not a real Elantech touchpad. Aborting.\n");
        return -1;
    }

    return 0;
}

int ApplePS2Elan::elantechQueryInfo() {
    unsigned char param[3];
    unsigned char traces;

    // Do the version query again so we can store the result
    if (synaptics_send_cmd<3>(ETP_FW_VERSION_QUERY, param)) {
        DEBUG_LOG("VoodooPS2Elan: failed to query firmware version.\n");
        return -1;
    }

    info.fw_version = (param[0] << 16) | (param[1] << 8) | param[2];

    if (elantechSetProperties()) {
        DEBUG_LOG("VoodooPS2Elan: unknown hardware version, aborting...\n");
        return -1;
    }

    DEBUG_LOG("VoodooPS2Elan assuming hardware version %d (with firmware version 0x%02x%02x%02x)\n",
           info.hw_version, param[0], param[1], param[2]);

    if (send_cmd<3>(ETP_CAPABILITIES_QUERY, info.capabilities)) {
        DEBUG_LOG("VoodooPS2Elan: failed to query capabilities.\n");
        return -1;
    }

    DEBUG_LOG("VoodooPS2Elan: Elan capabilities query result 0x%02x, 0x%02x, 0x%02x.\n",
           info.capabilities[0], info.capabilities[1],
           info.capabilities[2]);

    if (info.hw_version != 1) {
        if (send_cmd<3>(ETP_SAMPLE_QUERY, info.samples)) {
            DEBUG_LOG("VoodooPS2Elan: failed to query sample data\n");
            return -1;
        }
        DEBUG_LOG("VoodooPS2Elan: Elan sample query result %02x, %02x, %02x\n",
                      info.samples[0],
                      info.samples[1],
                      info.samples[2]);
    }

    if (info.samples[1] == 0x74 && info.hw_version == 0x03) {
        // This module has a bug which makes absolute mode unusable,
        // so let's abort so we'll be using standard PS/2 protocol.
        DEBUG_LOG("VoodooPS2Elan: absolute mode broken, forcing standard PS/2 protocol\n");
        return -1;
    }

    // The MSB indicates the presence of the trackpoint
    info.has_trackpoint = (info.capabilities[0] & 0x80) == 0x80;

    info.x_res = 31;
    info.y_res = 31;
    if (info.hw_version == 4) {
        if (elantech_get_resolution_v4(&info.x_res, &info.y_res, &info.bus)) {
            DEBUG_LOG("VoodooPS2Elan: failed to query resolution data.\n");
        }
        
        // ETD0180 BALANCED RESOLUTION: Optimal balance between smoothness and responsiveness
        if (IS_ETD0180()) {  // ETD0180
            info.x_res = 6;   // TEST: Resolution 6 with Resolution 5 dimensions to isolate the problem
            info.y_res = 6;   // TEST: Resolution 6 with Resolution 5 dimensions to isolate the problem
            IOLog("ETD0180_FIX: Balanced resolution x_res=%d y_res=%d for optimal smoothness + responsiveness\n", 
                  info.x_res, info.y_res);
        }
    }

    // query range information
    switch (info.hw_version) {
        case 1:
            info.x_min = ETP_XMIN_V1;
            info.y_min = ETP_YMIN_V1;
            info.x_max = ETP_XMAX_V1;
            info.y_max = ETP_YMAX_V1;
            break;

        case 2:
            if (info.fw_version == 0x020800 ||
                info.fw_version == 0x020b00 ||
                info.fw_version == 0x020030) {
                info.x_min = ETP_XMIN_V2;
                info.y_min = ETP_YMIN_V2;
                info.x_max = ETP_XMAX_V2;
                info.y_max = ETP_YMAX_V2;
            } else {
                if (send_cmd<3>(ETP_FW_ID_QUERY, param)) {
                    return -1;
                }

                int i = (info.fw_version > 0x020800 && info.fw_version < 0x020900) ? 1 : 2;
                int fixed_dpi = param[1] & 0x10;

                if (((info.fw_version >> 16) == 0x14) && fixed_dpi) {
                    if (send_cmd<3>(ETP_SAMPLE_QUERY, param)) {
                        return -1;
                    }

                    info.x_max = (info.capabilities[1] - i) * param[1] / 2;
                    info.y_max = (info.capabilities[2] - i) * param[2] / 2;
                } else if (info.fw_version == 0x040216) {
                    info.x_max = 819;
                    info.y_max = 405;
                } else if (info.fw_version == 0x040219 || info.fw_version == 0x040215) {
                    info.x_max = 900;
                    info.y_max = 500;
                } else {
                    info.x_max = (info.capabilities[1] - i) * 64;
                    info.y_max = (info.capabilities[2] - i) * 64;
                }
            }
            break;

        case 3:
            if (send_cmd<3>(ETP_FW_ID_QUERY, param)) {
                return -1;
            }

            info.x_max = (0x0f & param[0]) << 8 | param[1];
            info.y_max = (0xf0 & param[0]) << 4 | param[2];
            break;

        case 4:
            if (send_cmd<3>(ETP_FW_ID_QUERY, param)) {
                return -1;
            }

            info.x_max = (0x0f & param[0]) << 8 | param[1];
            info.y_max = (0xf0 & param[0]) << 4 | param[2];
            traces = info.capabilities[1];
            if ((traces < 2) || (traces > info.x_max)) {
                return -1;
            }

            info.width = info.x_max / (traces - 1);

            // column number of traces
            info.x_traces = traces;

            // row number of traces
            traces = info.capabilities[2];
            if ((traces >= 2) && (traces <= info.y_max)) {
                info.y_traces = traces;
            }

            break;
    }

    // check if device has buttonpad
    info.is_buttonpad = (info.fw_version & 0x001000) != 0;

    // check for the middle button
    info.has_middle_button = ETP_NEW_IC_SMBUS_HOST_NOTIFY(info.fw_version) && !info.is_buttonpad;

    return 0;
}

/*
 * determine hardware version and set some properties according to it.
 */
int ApplePS2Elan::elantechSetProperties() {
    // This represents the version of IC body
    int ver = (info.fw_version & 0x0f0000) >> 16;

    // Early version of Elan touchpads doesn't obey the rule
    if (info.fw_version < 0x020030 || info.fw_version == 0x020600) {
        info.hw_version = 1;
    } else {
        switch (ver) {
            case 2:
            case 4:
                info.hw_version = 2;
                break;
            case 5:
                info.hw_version = 3;
                break;
            case 6 ... 15:
                info.hw_version = 4;
                break;
            default:
                return -1;
        }
    }

    // Turn on packet checking by default
    info.paritycheck = 1;

    // This firmware suffers from misreporting coordinates when
    // a touch action starts causing the mouse cursor or scrolled page
    // to jump. Enable a workaround.
    info.jumpy_cursor = (info.fw_version == 0x020022 || info.fw_version == 0x020600);

    if (info.hw_version > 1) {
        // For now show extra debug information
        info.debug = 1;

        if (info.fw_version >= 0x020800) {
            info.reports_pressure = true;
        }
    }

    // The signatures of v3 and v4 packets change depending on the
    // value of this hardware flag.
    info.crc_enabled = (info.fw_version & 0x4000) == 0x4000;

    // Enable real hardware resolution on hw_version 3 ?
    info.set_hw_resolution = _set_hw_resolution;

    // Set packet length (4 for v1, 6 for v2 and newer)
    _packetLength = (info.hw_version == 1) ? 4 : 6;

    return 0;
}

/*
 * Set the appropriate event bits for the input subsystem
 */
int ApplePS2Elan::elantechSetInputParams() {
    setProperty(VOODOO_INPUT_LOGICAL_MAX_X_KEY, info.x_max - info.x_min, 32);
    setProperty(VOODOO_INPUT_LOGICAL_MAX_Y_KEY, info.y_max - info.y_min, 32);

    // EXPERIMENTAL: Test if Resolution 6 problem is caused by the physical dimension value 68266
    // Normal calculation would be: (info.x_max - info.x_min + 1) * 100 / info.x_res
    UInt32 physical_max_x = (info.x_max - info.x_min + 1) * 100 / info.x_res;
    UInt32 physical_max_y = (info.y_max - info.y_min + 1) * 100 / info.y_res;
    
    // CLOSER TO 5: Moving closer to Resolution 5 for less lag
    // Resolution 5 → 81920 → VoodooInput sees 16384 (0x4000)
    // 25000 is closer to 16384 = less laggy, closer to Resolution 5 feel
    if (info.x_res == 6) {
        physical_max_x = 17000;  // 16-bit safe, fine-tuned resolution  
        physical_max_y = 17000;  // Keep proportional  
        IOLog("ELAN_16BIT_FINETUNED: Using 17000 - 16-bit safe, fine-tuned resolution\n");
    }
    
    setProperty(VOODOO_INPUT_PHYSICAL_MAX_X_KEY, physical_max_x, 32);
    setProperty(VOODOO_INPUT_PHYSICAL_MAX_Y_KEY, physical_max_y, 32);
    
    IOLog("ELAN_DIMENSIONS: x_res=%d physical_max_x=%d physical_max_y=%d\n", 
          info.x_res, physical_max_x, physical_max_y);
    
    // Log button area configuration for calibration debugging
    UInt32 button_area_threshold = info.y_max - 100;
    IOLog("ELAN_BUTTON_AREA_CONFIG: Trackpad X=%d Y=%d, Button area threshold=%d (bottom %d units)\n", 
          info.x_max, info.y_max, button_area_threshold, 100);

    setProperty(VOODOO_INPUT_TRANSFORM_KEY, 0ull, 32);
    setProperty("VoodooInputSupported", kOSBooleanTrue);
    registerService();

    return 0;
}

/*
 * Put the touchpad into absolute mode
 */
int ApplePS2Elan::elantechSetAbsoluteMode() {
    unsigned char val;
    int tries = ETP_READ_BACK_TRIES;
    int rc = 0;

    switch (info.hw_version) {
        case 1:
            etd.reg_10 = 0x16;
            etd.reg_11 = 0x8f;
            if (elantechWriteReg(0x10, etd.reg_10) ||
                elantechWriteReg(0x11, etd.reg_11)) {
                rc = -1;
            }
            break;

        case 2:
            // Windows driver values
            etd.reg_10 = 0x54;
            etd.reg_11 = 0x88;    // 0x8a
            etd.reg_21 = 0x60;    // 0x00
            if (elantechWriteReg(0x10, etd.reg_10) ||
                elantechWriteReg(0x11, etd.reg_11) ||
                elantechWriteReg(0x21, etd.reg_21)) {
                rc = -1;
            }
            break;

        case 3:
            if (info.set_hw_resolution) {
                etd.reg_10 = 0x0b;
            } else {
                etd.reg_10 = 0x01;
            }

            if (elantechWriteReg(0x10, etd.reg_10)) {
                rc = -1;
            }

            break;

        case 4:
            etd.reg_07 = 0x01;
            if (elantechWriteReg(0x07, etd.reg_07)) {
                rc = -1;
            }

            goto skip_readback_reg_10; // v4 has no reg 0x10 to read
    }

    if (rc == 0) {
        // Read back reg 0x10. For hardware version 1 we must make
        // sure the absolute mode bit is set. For hardware version 2
        // the touchpad is probably initializing and not ready until
        // we read back the value we just wrote.
        do {
            rc = elantechReadReg(0x10, &val);
            if (rc == 0) {
                break;
            }
            tries--;
            DEBUG_LOG("VoodooPS2Elan: retrying read (%d).\n", tries);
            IOSleep(ETP_READ_BACK_DELAY);
        } while (tries > 0);

        if (rc) {
            DEBUG_LOG("VoodooPS2Elan: failed to read back register 0x10.\n");
        } else if (info.hw_version == 1 && !(val & ETP_R10_ABSOLUTE_MODE)) {
            DEBUG_LOG("VoodooPS2Elan: touchpad refuses to switch to absolute mode.\n");
            rc = -1;
        }
    }

skip_readback_reg_10:
    if (rc) {
        DEBUG_LOG("VoodooPS2Elan: failed to initialise registers.\n");
    }

    return rc;
}

/*
 * Initialize the touchpad
 */
int ApplePS2Elan::elantechSetupPS2() {
    DEBUG_LOG("VoodooPS2Elan: elantechSetupPS2() called!\n");
    DEBUG_LOG("VoodooPS2Elan: info.fw_version=0x%06x\n", info.fw_version);
    
    etd.parity[0] = 1;
    for (int i = 1; i < 256; i++)
        etd.parity[i] = etd.parity[i & (i - 1)] ^ 1;

    // Special handling for firmware 0x381f17 BEFORE trying absolute mode
    // This firmware has a bug where reg_07 gets cleared
    bool needs_reg07_fix = (IS_ETD0180());
    DEBUG_LOG("VoodooPS2Elan: needs_reg07_fix=%d\n", needs_reg07_fix);
    if (needs_reg07_fix) {
        IOLog("VoodooPS2Elan: ETD0180 detected (fw 0x381f17) - applying special handling\n");
        // Set etd.reg_07 to expected value for absolute mode
        etd.reg_07 = 0x01;
    }

    int absret = elantechSetAbsoluteMode();
    DEBUG_LOG("VoodooPS2Elan: elantechSetAbsoluteMode() returned %d\n", absret);
    if (absret) {
        if (needs_reg07_fix) {
            IOLog("VoodooPS2Elan: First absolute mode attempt failed for ETD0180, retrying with reg_07 fix\n");
            // Try to write reg_07 directly
            if (elantechWriteReg(0x07, 0x01) == 0) {
                IOLog("VoodooPS2Elan: Successfully wrote reg_07=0x01, retrying absolute mode\n");
                if (elantechSetAbsoluteMode() == 0) {
                    IOLog("VoodooPS2Elan: ETD0180 absolute mode enabled after reg_07 fix!\n");
                } else {
                    DEBUG_LOG("VoodooPS2Elan: ETD0180 still failed absolute mode after fix\n");
                    return -1;
                }
            } else {
                DEBUG_LOG("VoodooPS2Elan: Failed to write reg_07 for ETD0180\n");
                return -1;
            }
        } else {
            DEBUG_LOG("VoodooPS2: failed to put touchpad into absolute mode.\n");
            return -1;
        }
        } else if (needs_reg07_fix) {
        IOLog("VoodooPS2Elan: ETD0180 absolute mode set, ensuring reg_07 stays at 0x01\n");
        elantechWriteReg(0x07, 0x01);
    }

    // ETD0180 COORDINATE RANGE FIX: Use full hardware capability
    if (IS_ETD0180()) {
        // ANALYSIS: Live tests showed hardware can do much more:
        // ETD0180 V4 Protocol: Linux driver analysis shows v4 devices use 0-based coordinates
        // x_min=0, y_min=0 (native), x_max/y_max from firmware query
        // No coordinate transformation needed - hardware reports 0-based coordinates natively
        info.x_min = 0;      // V4 protocol uses 0-based X coordinates (Linux approach)
        info.x_max = 3094;   // Dynamic range - should query from firmware  
        info.y_min = 0;      // V4 protocol uses 0-based Y coordinates (Linux approach)
        info.y_max = 3096;   // Dynamic range - should query from firmware

        IOLog("VoodooPS2Elan: ETD0180 using FULL hardware ranges X=%d-%d, Y=%d-%d (range %d x %d)\n",
              info.x_min, info.x_max, info.y_min, info.y_max,
              info.x_max - info.x_min, info.y_max - info.y_min);
    }

    int inputret = elantechSetInputParams();
    DEBUG_LOG("VoodooPS2Elan: elantechSetInputParams() returned %d\n", inputret);
    if (inputret) {
        DEBUG_LOG("VoodooPS2: failed to query touchpad range.\n");
        return -1;
    }

    // set resolution and dpi
    TPS2Request<> request;
    request.commands[0].command = kPS2C_SendCommandAndCompareAck;
    request.commands[0].inOrOut = kDP_SetDefaultsAndDisable;           // 0xF5, Disable data reporting
    request.commands[1].command = kPS2C_SendCommandAndCompareAck;
    request.commands[1].inOrOut = kDP_SetMouseSampleRate;              // 0xF3
    request.commands[2].command = kPS2C_SendCommandAndCompareAck;
    request.commands[2].inOrOut = _mouseSampleRate;                    // 200 dpi
    request.commands[3].command = kPS2C_SendCommandAndCompareAck;
    request.commands[3].inOrOut = kDP_SetMouseResolution;              // 0xE8
    request.commands[4].command = kPS2C_SendCommandAndCompareAck;
    request.commands[4].inOrOut = _mouseResolution;                    // 0x03 = 8 counts/mm
    request.commands[5].command = kPS2C_SendCommandAndCompareAck;
    request.commands[5].inOrOut = kDP_SetMouseScaling1To1;             // 0xE6
    request.commands[6].command = kPS2C_SendCommandAndCompareAck;
    request.commands[6].inOrOut = kDP_Enable;                          // 0xF4, Enable Data Reporting
    request.commandsCount = 7;
    _device->submitRequestAndBlock(&request);
    
    // CRITICAL ETD0180 FIX: Restore absolute mode after PS2 initialization
    // ALL ETD0180 chips lose absolute mode after set_rate/set_resolution commands
    // This is NOT firmware-specific - it affects the entire ETD0180 series!
    if (IS_ETD0180()) {  // ETD0180 detection
        IOLog("VoodooPS2Elan: CRITICAL - ETD0180 absolute mode restoration required!\n");
        IOLog("VoodooPS2Elan: PS2 init destroys absolute mode - restoring reg_07=0x%02x\n", etd.reg_07);
        
        if (elantechWriteReg(0x07, etd.reg_07)) {
            IOLog("VoodooPS2Elan: ERROR - FAILED to restore reg_07! Trackpad in RELATIVE mode!\n");
            IOLog("VoodooPS2Elan: This explains small coordinate ranges - no absolute positioning!\n");
        } else {
            IOLog("VoodooPS2Elan: SUCCESS - reg_07 restored! Absolute mode should now be active!\n");
            IOLog("VoodooPS2Elan: Expecting full coordinate range: X=1014-3094, Y=1014-3096\n");
        }
    }

    return 0;
}

/*
 * Send an Elantech style special command to read a value from a register
 */
int ApplePS2Elan::elantechReadReg(unsigned char reg, unsigned char *val) {
    unsigned char param[3] = {0, 0, 0};
    int rc = 0;

    if (reg < 0x07 || reg > 0x26) {
        return -1;
    }

    if (reg > 0x11 && reg < 0x20) {
        return -1;
    }

    switch (info.hw_version) {
        case 1:
            if (ps2_sliced_command(ETP_REGISTER_READ) ||
                ps2_sliced_command(reg) ||
                ps2_command<3>(param, kDP_GetMouseInformation)) {
                rc = -1;
            }
            break;

        case 2:
            if (elantech_ps2_command<0>(NULL, ETP_PS2_CUSTOM_COMMAND) ||
                elantech_ps2_command<0>(NULL, ETP_REGISTER_READ) ||
                elantech_ps2_command<0>(NULL, ETP_PS2_CUSTOM_COMMAND) ||
                elantech_ps2_command<0>(NULL, reg) ||
                elantech_ps2_command<3>(param, kDP_GetMouseInformation)) {
                rc = -1;
            }
            break;

        case 3 ... 4:
            if (elantech_ps2_command<0>(NULL, ETP_PS2_CUSTOM_COMMAND) ||
                elantech_ps2_command<0>(NULL, ETP_REGISTER_READWRITE) ||
                elantech_ps2_command<0>(NULL, ETP_PS2_CUSTOM_COMMAND) ||
                elantech_ps2_command<0>(NULL, reg) ||
                elantech_ps2_command<3>(param, kDP_GetMouseInformation)) {
                rc = -1;
            }
            break;
    }

    if (rc) {
        DEBUG_LOG("VoodooPS2Elan: failed to read register 0x%02x.\n", reg);
    } else if (info.hw_version != 4) {
        *val = param[0];
    } else {
        *val = param[1];
    }

    return rc;
}

/*
 * Send an Elantech style special command to write a register with a value
 */
int ApplePS2Elan::elantechWriteReg(unsigned char reg, unsigned char val) {
    int rc = 0;

    if (reg < 0x07 || reg > 0x26) {
        return -1;
    }

    if (reg > 0x11 && reg < 0x20) {
        return -1;
    }

    switch (info.hw_version) {
        case 1:
            if (ps2_sliced_command(ETP_REGISTER_WRITE) ||
                ps2_sliced_command(reg) ||
                ps2_sliced_command(val) ||
                ps2_command<0>(NULL, kDP_SetMouseScaling1To1)) {
                rc = -1;
            }
            break;

        case 2:
            if (elantech_ps2_command<0>(NULL, ETP_PS2_CUSTOM_COMMAND) ||
                elantech_ps2_command<0>(NULL, ETP_REGISTER_WRITE) ||
                elantech_ps2_command<0>(NULL, ETP_PS2_CUSTOM_COMMAND) ||
                elantech_ps2_command<0>(NULL, reg) ||
                elantech_ps2_command<0>(NULL, ETP_PS2_CUSTOM_COMMAND) ||
                elantech_ps2_command<0>(NULL, val) ||
                elantech_ps2_command<0>(NULL, kDP_SetMouseScaling1To1)) {
                rc = -1;
            }
            break;

        case 3:
            if (elantech_ps2_command<0>(NULL, ETP_PS2_CUSTOM_COMMAND) ||
                elantech_ps2_command<0>(NULL, ETP_REGISTER_READWRITE) ||
                elantech_ps2_command<0>(NULL, ETP_PS2_CUSTOM_COMMAND) ||
                elantech_ps2_command<0>(NULL, reg) ||
                elantech_ps2_command<0>(NULL, ETP_PS2_CUSTOM_COMMAND) ||
                elantech_ps2_command<0>(NULL, val) ||
                elantech_ps2_command<0>(NULL, kDP_SetMouseScaling1To1)) {
                rc = -1;
            }
            break;

        case 4:
            if (elantech_ps2_command<0>(NULL, ETP_PS2_CUSTOM_COMMAND) ||
                elantech_ps2_command<0>(NULL, ETP_REGISTER_READWRITE) ||
                elantech_ps2_command<0>(NULL, ETP_PS2_CUSTOM_COMMAND) ||
                elantech_ps2_command<0>(NULL, reg) ||
                elantech_ps2_command<0>(NULL, ETP_PS2_CUSTOM_COMMAND) ||
                elantech_ps2_command<0>(NULL, ETP_REGISTER_READWRITE) ||
                elantech_ps2_command<0>(NULL, ETP_PS2_CUSTOM_COMMAND) ||
                elantech_ps2_command<0>(NULL, val) ||
                elantech_ps2_command<0>(NULL, kDP_SetMouseScaling1To1)) {
                rc = -1;
            }
            break;
    }

    if (rc) {
        DEBUG_LOG("VoodooPS2Elan: failed to write register 0x%02x with value 0x%02x.\n", reg, val);
    }

    return rc;
}

int ApplePS2Elan::elantechDebounceCheckV2() {
    // When we encounter packet that matches this exactly, it means the
    // hardware is in debounce status. Just ignore the whole packet.
    static const uint8_t debounce_packet[] = {
        0x84, 0xff, 0xff, 0x02, 0xff, 0xff
    };

    unsigned char *packet = _ringBuffer.tail();

    return !memcmp(packet, debounce_packet, sizeof(debounce_packet));
}

int ApplePS2Elan::elantechPacketCheckV1() {
    unsigned char *packet = _ringBuffer.tail();
    unsigned char p1, p2, p3;

    // Parity bits are placed differently
    if (info.fw_version < 0x020000) {
        // byte 0:  D   U  p1  p2   1  p3   R   L
        p1 = (packet[0] & 0x20) >> 5;
        p2 = (packet[0] & 0x10) >> 4;
    } else {
        // byte 0: n1  n0  p2  p1   1  p3   R   L
        p1 = (packet[0] & 0x10) >> 4;
        p2 = (packet[0] & 0x20) >> 5;
    }

    p3 = (packet[0] & 0x04) >> 2;

    return etd.parity[packet[1]] == p1 &&
           etd.parity[packet[2]] == p2 &&
           etd.parity[packet[3]] == p3;
}

int ApplePS2Elan::elantechPacketCheckV2() {
    unsigned char *packet = _ringBuffer.tail();

    // V2 hardware has two flavors. Older ones that do not report pressure,
    // and newer ones that reports pressure and width. With newer ones, all
    // packets (1, 2, 3 finger touch) have the same constant bits. With
    // older ones, 1/3 finger touch packets and 2 finger touch packets
    // have different constant bits.
    // With all three cases, if the constant bits are not exactly what I
    // expected, I consider them invalid.

    if (info.reports_pressure) {
        return (packet[0] & 0x0c) == 0x04 && (packet[3] & 0x0f) == 0x02;
    }

    if ((packet[0] & 0xc0) == 0x80) {
        return (packet[0] & 0x0c) == 0x0c && (packet[3] & 0x0e) == 0x08;
    }

    return (packet[0] & 0x3c) == 0x3c &&
           (packet[1] & 0xf0) == 0x00 &&
           (packet[3] & 0x3e) == 0x38 &&
           (packet[4] & 0xf0) == 0x00;
}

int ApplePS2Elan::elantechPacketCheckV3() {
    static const uint8_t debounce_packet[] = {
        0xc4, 0xff, 0xff, 0x02, 0xff, 0xff
    };

    unsigned char *packet = _ringBuffer.tail();

    // check debounce first, it has the same signature in byte 0
    // and byte 3 as PACKET_V3_HEAD.
    if (!memcmp(packet, debounce_packet, sizeof(debounce_packet))) {
        return PACKET_DEBOUNCE;
    }

    // If the hardware flag 'crc_enabled' is set the packets have different signatures.
    if (info.crc_enabled) {
        if ((packet[3] & 0x09) == 0x08) {
            return PACKET_V3_HEAD;
        }

        if ((packet[3] & 0x09) == 0x09) {
            return PACKET_V3_TAIL;
        }
    } else {
        if ((packet[0] & 0x0c) == 0x04 && (packet[3] & 0xcf) == 0x02) {
            return PACKET_V3_HEAD;
        }

        if ((packet[0] & 0x0c) == 0x0c && (packet[3] & 0xce) == 0x0c) {
            return PACKET_V3_TAIL;
        }

        if ((packet[3] & 0x0f) == 0x06) {
            return PACKET_TRACKPOINT;
        }
    }

    return PACKET_UNKNOWN;
}

void ApplePS2Elan::elantechRescale(unsigned int &x, unsigned int &y) {
    bool needs_update = false;

    if (x > info.x_max) {
        info.x_max = x;
        needs_update = true;
    }
    if (x < info.x_min) {
        info.x_min = x;
        needs_update = true;
    }

    if (y > info.y_max) {
        info.y_max = y;
        needs_update = true;
    }
    if (y < info.y_min) {
        info.y_min = y;
        needs_update = true;
    }

    if (needs_update) {
        setProperty(VOODOO_INPUT_LOGICAL_MAX_X_KEY, info.x_max - info.x_min, 32);
        setProperty(VOODOO_INPUT_LOGICAL_MAX_Y_KEY, info.y_max - info.y_min, 32);

        // EXPERIMENTAL: Test if Resolution 6 problem is caused by the physical dimension value 68266
        UInt32 physical_max_x = (info.x_max - info.x_min + 1) * 100 / info.x_res;
        UInt32 physical_max_y = (info.y_max - info.y_min + 1) * 100 / info.y_res;
        
        // OPTIMAL: Resolution 5.8 equivalent - creates VoodooInput-friendly dimension value 70620
        if (info.x_res == 6) {
            physical_max_x = 70620;  // Resolution 5.8 equivalent: 4096*100/5.8 = 70620
            physical_max_y = 70620;  // Keep proportional
        }
        
        setProperty(VOODOO_INPUT_PHYSICAL_MAX_X_KEY, physical_max_x, 32);
        setProperty(VOODOO_INPUT_PHYSICAL_MAX_Y_KEY, physical_max_y, 32);

        if (voodooInputInstance) {
            VoodooInputDimensions dims = {
                .min_x = static_cast<SInt32>(info.x_min),
                .max_x = static_cast<SInt32>(info.x_max),
                .min_y = static_cast<SInt32>(info.y_min),
                .max_y = static_cast<SInt32>(info.y_max)
            };
            super::messageClient(kIOMessageVoodooInputUpdateDimensionsMessage, voodooInputInstance, &dims, sizeof(VoodooInputDimensions));
        }

        IOLog("VoodooPS2Elan: rescaled logical range to %dx%d, physical %dx%d\n",
            info.x_max - info.x_min, info.y_max - info.y_min,
            physical_max_x, physical_max_y);
    }
}

int ApplePS2Elan::elantechPacketCheckV4() {
    unsigned char *packet = _ringBuffer.tail();
    unsigned char packet_type = packet[3] & 0x03;
    unsigned int ic_version;
    bool sanity_check;

    // CALIBRATION: Complete packet logging for comprehensive analysis
    DEBUG_LOG("VoodooPS2Elan: Received packet for fw=0x%06x\n", info.fw_version);

    if (info.has_trackpoint && (packet[3] & 0x0f) == 0x06) {
        return PACKET_TRACKPOINT;
    }

    // ETD0180 debug logging but treat as normal V4 hardware
    if (IS_ETD0180()) {
        DEBUG_LOG("ETD0180: Processing packet\n");
        DEBUG_LOG("ETD0180: packet_type=%d\n", packet_type);
        
        // Check all possible multi-touch indicators
        if (packet_type == 0) {
            DEBUG_LOG("ETD0180: STATUS packet detected\n");
        } else if (packet_type == 1) {
            DEBUG_LOG("ETD0180: HEAD packet detected! (RARE for ETD0180)\n");
        } else if (packet_type == 2) {
            DEBUG_LOG("ETD0180: MOTION packet detected\n");
            // Check if this might be a multi-touch MOTION packet
            if ((packet[0] & 0x30) == 0x20) {
                DEBUG_LOG("ETD0180: Possible SECOND finger MOTION\n");
            } else if ((packet[0] & 0x30) == 0x10) {
                DEBUG_LOG("ETD0180: Possible FIRST finger MOTION\n");
            } else if ((packet[0] & 0x30) == 0x30) {
                DEBUG_LOG("ETD0180: Possible DUAL finger MOTION\n");
            }
        }
    }

    // This represents the version of IC body.
    ic_version = (info.fw_version & 0x0f0000) >> 16;

    INTERRUPT_LOG("VoodooPS2Elan: icVersion(%d), crc(%d), samples[1](%d) \n", ic_version, info.crc_enabled, info.samples[1]);

    // Sanity check based on the constant bits of a packet.
    // The constant bits change depending on the value of
    // the hardware flag 'crc_enabled' and the version of
    // the IC body, but are the same for every packet,
    // regardless of the type.
    if (info.crc_enabled) {
        sanity_check = ((packet[3] & 0x08) == 0x00);
    } else if (ic_version == 7 && info.samples[1] == 0x2A) {
        sanity_check = ((packet[3] & 0x1c) == 0x10);
    } else {
        sanity_check = ((packet[0] & 0x08) == 0x00 && (packet[3] & 0x1c) == 0x10);
    }

    if (!sanity_check) {
        return PACKET_UNKNOWN;
    }

    switch (packet_type) {
        case 0:
            return PACKET_V4_STATUS;

        case 1:
            return PACKET_V4_HEAD;

        case 2:
            return PACKET_V4_MOTION;
    }

    return PACKET_UNKNOWN;
}


void ApplePS2Elan::elantechReportAbsoluteV1() {
    unsigned char *packet = _ringBuffer.tail();
    unsigned int fingers = 0, x = 0, y = 0;

    if (info.fw_version < 0x020000) {
        // byte 0:  D   U  p1  p2   1  p3   R   L
        // byte 1:  f   0  th  tw  x9  x8  y9  y8
        fingers = ((packet[1] & 0x80) >> 7) + ((packet[1] & 0x30) >> 4);
    } else {
        // byte 0: n1  n0  p2  p1   1  p3   R   L
        // byte 1:  0   0   0   0  x9  x8  y9  y8
        fingers = (packet[0] & 0xc0) >> 6;
    }

    if (info.jumpy_cursor) {
        if (fingers != 1) {
            etd.single_finger_reports = 0;
        } else if (etd.single_finger_reports < 2) {
            // Discard first 2 reports of one finger, bogus
            etd.single_finger_reports++;
            INTERRUPT_LOG("VoodooPS2Elan: discarding packet\n");
            return;
        }
    }

    // byte 2: x7  x6  x5  x4  x3  x2  x1  x0
    // byte 3: y7  y6  y5  y4  y3  y2  y1  y0
    x = ((packet[1] & 0x0c) << 6) | packet[2];
    y = info.y_max - (((packet[1] & 0x03) << 8) | packet[3]);

    virtualFinger[0].touch = false;
    virtualFinger[1].touch = false;
    virtualFinger[2].touch = false;

    leftButton = packet[0] & 0x01;
    rightButton = packet[0] & 0x02;

    if (fingers == 1) {
        virtualFinger[0].touch = true;
        virtualFinger[0].button = packet[0] & 0x03;
        virtualFinger[0].prev = virtualFinger[0].now;
        virtualFinger[0].now.x = x;
        virtualFinger[0].now.y = y;
        if (lastFingers != 1) {
            virtualFinger[0].prev = virtualFinger[0].now;
        }
    }

    if (fingers == 2) {
        virtualFinger[0].touch = virtualFinger[1].touch = true;
        virtualFinger[0].button = virtualFinger[1].button = packet[0] & 0x03;
        virtualFinger[0].prev = virtualFinger[0].now;
        virtualFinger[1].prev = virtualFinger[1].now;

        int h = 100;
        int dy = (int)(sin30deg * h);
        int dx = (int)(cos30deg * h);

        virtualFinger[0].now.x = x;
        virtualFinger[0].now.y = y - h;

        virtualFinger[1].now.x = x + dx;
        virtualFinger[1].now.y = y + dy;

        if (lastFingers != 2) {
            virtualFinger[0].prev = virtualFinger[0].now;
            virtualFinger[1].prev = virtualFinger[1].now;
        }
    }

    if (fingers == 3) {
        virtualFinger[0].touch = virtualFinger[1].touch = virtualFinger[2].touch = true;
        virtualFinger[0].button = virtualFinger[1].button = virtualFinger[2].button = packet[0] & 0x03;
        virtualFinger[0].prev = virtualFinger[0].now;
        virtualFinger[1].prev = virtualFinger[1].now;
        virtualFinger[2].prev = virtualFinger[2].now;

        int h = 100;
        int dy = (int)(sin30deg * h);
        int dx = (int)(cos30deg * h);

        virtualFinger[0].now.x = x;
        virtualFinger[0].now.y = y - h;

        virtualFinger[1].now.x = x - dx;
        virtualFinger[1].now.y = y + dy;

        virtualFinger[2].now.x = x + dx;
        virtualFinger[2].now.y = y + dy;

        if (lastFingers != 3) {
            virtualFinger[0].prev = virtualFinger[0].now;
            virtualFinger[1].prev = virtualFinger[1].now;
            virtualFinger[2].prev = virtualFinger[2].now;
        }
    }

    lastFingers = fingers;
    sendTouchData();
}

void ApplePS2Elan::elantechReportAbsoluteV2() {
    unsigned char *packet = _ringBuffer.tail();
    unsigned int fingers = 0, x1 = 0, y1 = 0, x2 = 0, y2 = 0;

    // byte 0: n1  n0   .   .   .   .   R   L
    fingers = (packet[0] & 0xc0) >> 6;

    switch (fingers) {
        case 3:
        case 1:
            // byte 1:  .   .   .   .  x11 x10 x9  x8
            // byte 2: x7  x6  x5  x4  x4  x2  x1  x0
            x1 = ((packet[1] & 0x0f) << 8) | packet[2];

            // byte 4:  .   .   .   .  y11 y10 y9  y8
            // byte 5: y7  y6  y5  y4  y3  y2  y1  y0
            y1 = info.y_max - (((packet[4] & 0x0f) << 8) | packet[5]);

            // pressure: (packet[1] & 0xf0) | ((packet[4] & 0xf0) >> 4);
            // finger width: ((packet[0] & 0x30) >> 2) | ((packet[3] & 0x30) >> 4);
            break;

        case 2:
            // The coordinate of each finger is reported separately
            // with a lower resolution for two finger touches:

            // byte 0:  .   .  ay8 ax8  .   .   .   .
            // byte 1: ax7 ax6 ax5 ax4 ax3 ax2 ax1 ax0
            x1 = (((packet[0] & 0x10) << 4) | packet[1]) << 2;

            // byte 2: ay7 ay6 ay5 ay4 ay3 ay2 ay1 ay0
            y1 = info.y_max - ((((packet[0] & 0x20) << 3) | packet[2]) << 2);

            // byte 3:  .   .  by8 bx8  .   .   .   .
            // byte 4: bx7 bx6 bx5 bx4 bx3 bx2 bx1 bx0
            x2 = (((packet[3] & 0x10) << 4) | packet[4]) << 2;

            // byte 5: by7 by8 by5 by4 by3 by2 by1 by0
            y2 = info.y_max - ((((packet[3] & 0x20) << 3) | packet[5]) << 2);
            break;
    }

    virtualFinger[0].touch = false;
    virtualFinger[1].touch = false;
    virtualFinger[2].touch = false;

    leftButton = packet[0] & 0x01;
    rightButton = packet[0] & 0x02;

    if (fingers == 1 || fingers == 2) {
        virtualFinger[0].touch = true;
        virtualFinger[0].button = packet[0] & 0x03;
        virtualFinger[0].prev = virtualFinger[0].now;
        virtualFinger[0].now.x = x1;
        virtualFinger[0].now.y = y1;
        if (lastFingers != 1 && lastFingers != 2) {
            virtualFinger[0].prev = virtualFinger[0].now;
        }
    }

    if (fingers == 2) {
        virtualFinger[1].touch = true;
        virtualFinger[1].button = packet[0] & 0x03;
        virtualFinger[1].prev = virtualFinger[1].now;
        virtualFinger[1].now.x = x2;
        virtualFinger[1].now.y = y2;
        if (lastFingers != 2) {
            virtualFinger[1].prev = virtualFinger[1].now;
        }
    }

    if (fingers == 3) {
        virtualFinger[0].touch = virtualFinger[1].touch = virtualFinger[2].touch = true;
        virtualFinger[0].button = virtualFinger[1].button = virtualFinger[2].button = packet[0] & 0x03;
        virtualFinger[0].prev = virtualFinger[0].now;
        virtualFinger[1].prev = virtualFinger[1].now;
        virtualFinger[2].prev = virtualFinger[2].now;

        int h = 100;
        int dy = (int)(sin30deg * h);
        int dx = (int)(cos30deg * h);

        virtualFinger[0].now.x = x1;
        virtualFinger[0].now.y = y1 - h;

        virtualFinger[1].now.x = x1 - dx;
        virtualFinger[1].now.y = y1 + dy;

        virtualFinger[2].now.x = x1 + dx;
        virtualFinger[2].now.y = y1 + dy;

        if (lastFingers != 3) {
            virtualFinger[0].prev = virtualFinger[0].now;
            virtualFinger[1].prev = virtualFinger[1].now;
            virtualFinger[2].prev = virtualFinger[2].now;
        }
    }

    lastFingers = fingers;
    sendTouchData();
}

void ApplePS2Elan::elantechReportAbsoluteV3(int packetType) {
    unsigned char *packet = _ringBuffer.tail();
    unsigned int fingers = 0, x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    
    // byte 0: n1  n0   .   .   .   .   R   L
    fingers = (packet[0] & 0xc0) >> 6;

    INTERRUPT_LOG("report abs v3 type %d finger %u x %d y %d btn %d (%02x %02x %02x %02x %02x %02x)\n", packetType, fingers,
                  ((packet[1] & 0x0f) << 8) | packet[2],
                  (((packet[4] & 0x0f) << 8) | packet[5]),
                  packet[0] & 0x03, packet[0], packet[1], packet[2], packet[3], packet[4], packet[5]);

    switch (fingers) {
        case 3:
        case 1:
            // byte 1:  .   .   .   .  x11 x10 x9  x8
            // byte 2: x7  x6  x5  x4  x4  x2  x1  x0
            x1 = ((packet[1] & 0x0f) << 8) | packet[2];

            // byte 4:  .   .   .   .  y11 y10 y9  y8
            // byte 5: y7  y6  y5  y4  y3  y2  y1  y0
            y1 = (((packet[4] & 0x0f) << 8) | packet[5]);
            elantechRescale(x1, y1);
            y1 = info.y_max - y1;
            break;

        case 2:
            if (packetType == PACKET_V3_HEAD) {
                // byte 1:   .    .    .    .  ax11 ax10 ax9  ax8
                // byte 2: ax7  ax6  ax5  ax4  ax3  ax2  ax1  ax0
                etd.mt[0].x = ((packet[1] & 0x0f) << 8) | packet[2];

                // byte 4:   .    .    .    .  ay11 ay10 ay9  ay8
                // byte 5: ay7  ay6  ay5  ay4  ay3  ay2  ay1  ay0
                etd.mt[0].y = info.y_max - (((packet[4] & 0x0f) << 8) | packet[5]);

                // wait for next packet
                return;
            }

            // packet_type == PACKET_V3_TAIL
            x1 = etd.mt[0].x;
            y1 = etd.mt[0].y;
            x2 = ((packet[1] & 0x0f) << 8) | packet[2];
            y2 = (((packet[4] & 0x0f) << 8) | packet[5]);
            elantechRescale(x2, y2);
            y2 = info.y_max - y2;
            break;
    }

    // pressure: (packet[1] & 0xf0) | ((packet[4] & 0xf0) >> 4);
    // finger width: ((packet[0] & 0x30) >> 2) | ((packet[3] & 0x30) >> 4);

    virtualFinger[0].touch = false;
    virtualFinger[1].touch = false;
    virtualFinger[2].touch = false;

    leftButton = packet[0] & 0x01;
    rightButton = packet[0] & 0x02;

    if (fingers == 1 || fingers == 2) {
        virtualFinger[0].touch = true;
        virtualFinger[0].button = packet[0] & 0x03;
        virtualFinger[0].prev = virtualFinger[0].now;
        virtualFinger[0].now.x = x1;
        virtualFinger[0].now.y = y1;
        if (lastFingers != 1 && lastFingers != 2) {
            virtualFinger[0].prev = virtualFinger[0].now;
        }
    }

    if (fingers == 2) {
        virtualFinger[1].touch = true;
        virtualFinger[1].button = packet[0] & 0x03;
        virtualFinger[1].prev = virtualFinger[1].now;
        virtualFinger[1].now.x = x2;
        virtualFinger[1].now.y = y2;
        if (lastFingers != 2) {
            virtualFinger[1].prev = virtualFinger[1].now;
        }
    }

    if (fingers == 3) {
        virtualFinger[0].touch = virtualFinger[1].touch = virtualFinger[2].touch = true;
        virtualFinger[0].button = virtualFinger[1].button = virtualFinger[2].button = packet[0] & 0x03;
        virtualFinger[0].prev = virtualFinger[0].now;
        virtualFinger[1].prev = virtualFinger[1].now;
        virtualFinger[2].prev = virtualFinger[2].now;

        int h = 100;
        int dy = (int)(sin30deg * h);
        int dx = (int)(cos30deg * h);

        virtualFinger[0].now.x = x1;
        virtualFinger[0].now.y = y1 - h;

        virtualFinger[1].now.x = x1 - dx;
        virtualFinger[1].now.y = y1 + dy;

        virtualFinger[2].now.x = x1 + dx;
        virtualFinger[2].now.y = y1 + dy;

        if (lastFingers != 3) {
            virtualFinger[0].prev = virtualFinger[0].now;
            virtualFinger[1].prev = virtualFinger[1].now;
            virtualFinger[2].prev = virtualFinger[2].now;
        }
    }

    lastFingers = fingers;
    sendTouchData();
}

void ApplePS2Elan::elantechReportAbsoluteV4(int packetType) {
    if (IS_ETD0180()) {
        if (IS_ETD0180()) {
        DEBUG_LOG("[ETD0180_PROCESS] PacketType=%d\n", packetType);
    } else {
        DEBUG_LOG("ETD0180: type=%d (0=STATUS 1=HEAD 2=MOTION)\n", packetType);
    }
    }
    
    switch (packetType) {
        case PACKET_V4_STATUS:
            processPacketStatusV4();
            break;

        case PACKET_V4_HEAD:
            processPacketHeadV4();
            break;

        case PACKET_V4_MOTION:
            processPacketMotionV4();
            break;

        case PACKET_UNKNOWN:
        default:
            IOLog("VoodooPS2Elan: Got UNKNOWN packet type %d\n", packetType);
            break;
    }
}

void ApplePS2Elan::elantechReportTrackpoint() {
    // byte 0:   0   0  sx  sy   0   M   R   L
    // byte 1: ~sx   0   0   0   0   0   0   0
    // byte 2: ~sy   0   0   0   0   0   0   0
    // byte 3:   0   0 ~sy ~sx   0   1   1   0
    // byte 4:  x7  x6  x5  x4  x3  x2  x1  x0
    // byte 5:  y7  y6  y5  y4  y3  y2  y1  y0
    //
    // x and y are written in two's complement spread
    // over 9 bits with sx/sy the relative top bit and
    // x7..x0 and y7..y0 the lower bits.
    // ~sx is the inverse of sx, ~sy is the inverse of sy.
    // The sign of y is opposite to what the input driver
    // expects for a relative movement

    UInt32 *t = (UInt32 *)_ringBuffer.tail();
    UInt32 signature = *t & ~7U;
    if (signature != 0x06000030U &&
        signature != 0x16008020U &&
        signature != 0x26800010U &&
        signature != 0x36808000U) {
        INTERRUPT_LOG("VoodooPS2Elan: unexpected trackpoint packet skipped\n");
        return;
    }

    unsigned char *packet = _ringBuffer.tail();


    // Use packet to avoid compiler warning
    (void)packet;

    // remember last time trackpoint was used. this can be used in
    // interrupt handler to detect unintended input
    keytime = 0; // Simplified timestamp
    
    // DISABLED: Trackpoint messages cause VoodooInput to create TrackpointDevice instead of multitouch trackpad
    IOLog("VoodooPS2Elan: Trackpoint message disabled - ELAN touchpad should use multitouch only\n");
}

void ApplePS2Elan::processPacketStatusV4() {
    unsigned char *packet = _ringBuffer.tail();
    unsigned fingers;
    leftButton = packet[0] & 0x1;
    rightButton = packet[0] & 0x2;
    
    // ETD0180 STATUS packet handling
    if (IS_ETD0180()) {
        static int status_pkt_num = 0;
        status_pkt_num++;
        
        DEBUG_LOG("[ETD0180_STATUS_%04d] RAW[0x%02x 0x%02x 0x%02x 0x%02x 0x%02x 0x%02x]\n",
              status_pkt_num, packet[0], packet[1], packet[2], packet[3], packet[4], packet[5]);
        
        int fingerBits = (packet[0] & 0x30) >> 4;
        fingers = packet[1] & 0x1f;
        
        DEBUG_LOG("[ETD0180_STATUS_%04d] finger_count=%d fingerBits=0x%x\n", 
              status_pkt_num, fingers, fingerBits);
        
        // Clear all fingers on STATUS packet (finger lift)
        if (fingers == 0) {
            DEBUG_LOG("[ETD0180_STATUS_%04d] All fingers lifted - clearing touch state\n", status_pkt_num);
            for (int i = 0; i < ETP_MAX_FINGERS; i++) {
                virtualFinger[i].touch = false;
            }
            sendTouchData();
            return;
        }
    }

    // notify finger state change
    int count = 0;
    
    fingers = packet[1] & 0x1f;
    
    for (int i = 0; i < ETP_MAX_FINGERS; i++) {
        if ((fingers & (1 << i)) == 0) {
            // finger has been lifted off the touchpad
            if (virtualFinger[i].touch) {
                IOLog("VoodooPS2Elan: %d finger has been lifted off the touchpad\n", i);
            }
            virtualFinger[i].touch = false;
        } else {
            virtualFinger[i].touch = true;
            count++;
        }
    }

    heldFingers = count;
    headPacketsCount = 0;
    
    // if count > 0, we wait for HEAD packets to report so that we report all fingers at once.
    // if count == 0, we have to report the fact fingers are taken off, because there won't be any HEAD packets
    if (count == 0) {
        sendTouchData();
    }
}

void ApplePS2Elan::processPacketHeadV4() {
    unsigned char *packet = _ringBuffer.tail();
    int id;
    int pres, traces;

    // ETD0180 special HEAD packet handling for Multi-Touch
    if (IS_ETD0180()) {
        static int head_pkt_num = 0;
        head_pkt_num++;
        
        DEBUG_LOG("[ETD0180_HEAD_%04d] RAW[0x%02x 0x%02x 0x%02x 0x%02x 0x%02x 0x%02x]\n",
              head_pkt_num, packet[0], packet[1], packet[2], packet[3], packet[4], packet[5]);
        
        // ETD0180: HEAD packets are RARE! Usually uses MOTION packets
        // When HEAD occurs, it's usually single touch
        int fingerBits = (packet[0] & 0x30) >> 4;
        
        // For HEAD packets, always use standard V4 finger ID extraction
        // HEAD packets don't follow the fbits pattern like MOTION
        id = ((packet[3] & 0xe0) >> 5) - 1;
        if (id < 0 || id >= ETP_MAX_FINGERS) {
            id = 0;  // Default to F0
        }
        
        DEBUG_LOG("[ETD0180_HEAD_%04d] RARE HEAD packet! id=%d fbits=0x%x\n", 
              head_pkt_num, id, fingerBits);
    } else {
        // Standard V4 processing for non-ETD0180
        id = ((packet[3] & 0xe0) >> 5) - 1;
    }

    leftButton = packet[0] & 0x1;
    rightButton = packet[0] & 0x2;
    headPacketsCount++;
    
    // Validate finger ID
    if (id < 0 || id >= ETP_MAX_FINGERS) {
        if (IS_ETD0180()) {
            DEBUG_LOG("[ETD0180_HEAD_ERROR] Invalid finger ID %d, dropping packet\n", id);
        }
        return;
    }
    
    if (IS_ETD0180()) {
        DEBUG_LOG("[ETD0180_HEAD_FINGER] Processing for finger F%d\n", id);
    }

    int x = ((packet[1] & 0x0f) << 8) | packet[2];
    int y = info.y_max - (((packet[4] & 0x0f) << 8) | packet[5]);
    
    // Coordinate extraction debug for ETD0180
    if (IS_ETD0180()) {
        DEBUG_LOG("[ETD0180_COORDS] Finger %d:\n", id);
        IOLog("  - X: packet[1]&0x0f=0x%02x << 8 | packet[2]=0x%02x = %d\n",
              packet[1] & 0x0f, packet[2], x);
        IOLog("  - Y: packet[4]&0x0f=0x%02x << 8 | packet[5]=0x%02x = %d (inverted=%d)\n",
              packet[4] & 0x0f, packet[5], ((packet[4] & 0x0f) << 8) | packet[5], y);
    }

    pres = (packet[1] & 0xf0) | ((packet[4] & 0xf0) >> 4);
    traces = (packet[0] & 0xf0) >> 4;

    if (IS_ETD0180()) {
        DEBUG_LOG("[ETD0180_TOUCH] F%d: X=%d Y=%d pres=%d traces=%d btn=%d\n", 
              id, x, y, pres, traces, packet[0] & 0x3);
    }
    
    INTERRUPT_LOG("VoodooPS2Elan: pres: %d, traces: %d, width: %d\n", pres, traces, traces);

    virtualFinger[id].button = (packet[0] & 0x3);
    virtualFinger[id].prev = virtualFinger[id].now;
    virtualFinger[id].pressure = pres;
    virtualFinger[id].width = traces;
    virtualFinger[id].touch = 1;  // Mark finger as active

    virtualFinger[id].now.x = x;
    virtualFinger[id].now.y = y;
    
    // REMOVED EDGE DETECTION: VoodooInput handles this natively with correct dimensions

    // ETD0180 LINUX IMPLEMENTATION - Use traces only for touch area like Linux kernel
    if (IS_ETD0180()) {
        // Linux approach: traces is only used for touch area calculation, not finger detection
        // virtualFinger[id].width already set above to traces (same as Linux)
        DEBUG_LOG("[ETD0180_LINUX] HEAD packet - id=%d, x=%d, y=%d, traces=%d (touch_area_only)\n", 
              id, x, y, traces);
    }

    // ETD0180: HEAD packets are rare, send immediately
    if (IS_ETD0180()) {
        DEBUG_LOG("[ETD0180_HEAD_SEND] Sending HEAD packet data immediately\n");
        sendTouchData();
    } else {
        // Standard V4: Wait for all HEAD packets
        if (headPacketsCount == heldFingers) {
            headPacketsCount = 0;
            sendTouchData();
        }
    }
}

void ApplePS2Elan::processPacketMotionV4() {
    unsigned char *packet = _ringBuffer.tail();
    
    // ETD0180 LINUX IMPLEMENTATION: Use standard V4 MOTION processing (no special case)
    if (IS_ETD0180()) {
        DEBUG_LOG("[ETD0180_LINUX] Using standard V4 motion processing - no special handling\n");
    }
    
    // Standard V4 MOTION packet processing (relative deltas)
    int weight, delta_x1 = 0, delta_y1 = 0, delta_x2 = 0, delta_y2 = 0;
    int id, sid;

    leftButton = packet[0] & 0x1;
    rightButton = packet[0] & 0x2;

    id = ((packet[0] & 0xe0) >> 5) - 1;
    if (id < 0) {
        INTERRUPT_LOG("VoodooPS2Elan: invalid id, aborting\n");
        return;
    }

    sid = ((packet[3] & 0xe0) >> 5) - 1;
    weight = (packet[0] & 0x10) ? ETP_WEIGHT_VALUE : 1;

    // Motion packets give us the delta of x, y values of specific fingers,
    // but in two's complement. Let the compiler do the conversion for us.
    // Also _enlarge_ the numbers to int, in case of overflow.
    delta_x1 = (signed char)packet[1];
    delta_y1 = (signed char)packet[2];
    delta_x2 = (signed char)packet[4];
    delta_y2 = (signed char)packet[5];

    virtualFinger[id].button = (packet[0] & 0x3);
    virtualFinger[id].prev = virtualFinger[id].now;
    virtualFinger[id].now.x += delta_x1 * weight;
    virtualFinger[id].now.y -= delta_y1 * weight;

    if (sid >= 0) {
        virtualFinger[sid].button = (packet[0] & 0x3);
        virtualFinger[sid].prev = virtualFinger[sid].now;
        virtualFinger[sid].now.x += delta_x2 * weight;
        virtualFinger[sid].now.y -= delta_y2 * weight;
    }

    sendTouchData();
}

void ApplePS2Elan::processPacketETD0180() {
    unsigned char *packet = _ringBuffer.tail();
    
    // ETD0180 button extraction
    leftButton = packet[0] & 0x1;
    rightButton = packet[0] & 0x2;
    
    // ETD0180 coordinate extraction - USE ETD0180 METHOD, NOT V4 HEAD!
    // According to ETD0180_Packet_Matrix.md:
    unsigned int x = packet[1] | ((packet[3] & 0x0F) << 8);
    unsigned int y = packet[2] | ((packet[4] & 0x0F) << 8);
    
    DEBUG_LOG("ETD0180_COORDS: [0x%02x 0x%02x 0x%02x 0x%02x 0x%02x 0x%02x] X=%d Y=%d L%d/R%d\n", 
          packet[0], packet[1], packet[2], packet[3], packet[4], packet[5], x, y, leftButton, rightButton);
    
    elantechRescale(x, y);
    
    // Finger presence detection - ETD0180 uses different bits than standard V4
    // Check if we have valid coordinates and finger presence
    bool fingerPresent = (packet[0] & 0x30) != 0;  // bits 4-5 indicate finger presence
    
    if (fingerPresent && x > 0 && y > 0 && x < info.x_max && y < info.y_max) {
        // Valid touch detected
        virtualFinger[0].touch = true;
        virtualFinger[0].prev = virtualFinger[0].now;
        virtualFinger[0].now.x = x;
        virtualFinger[0].now.y = info.y_max - y; // Invert Y for macOS
        virtualFinger[0].button = (leftButton ? 1 : 0) | (rightButton ? 2 : 0);
        
        DEBUG_LOG("ETD0180_TOUCH: Valid touch at X=%d Y=%d (inverted Y=%d)\n", 
              x, y, (int)virtualFinger[0].now.y);
    } else {
        // No finger or invalid coordinates
        virtualFinger[0].touch = false;
        virtualFinger[0].prev = virtualFinger[0].now;
        
        DEBUG_LOG("ETD0180_NO_TOUCH: finger=%d x=%d y=%d (max X=%d Y=%d)\n", 
              fingerPresent, x, y, info.x_max, info.y_max);
    }
    
    
    sendTouchData();
}

void ApplePS2Elan::processPacketETD0180MultiTouch(int packetType) {
    unsigned char *packet = _ringBuffer.tail();
    
    // ETD0180 button extraction (only from first finger packet)
    if (packetType == 5) { // V4_HEAD - first finger
        leftButton = packet[0] & 0x1;
        rightButton = packet[0] & 0x2;
    }
    
    // Determine finger index based on packet type
    int fingerIndex = 0;
    if (packetType == 5) {      // V4_HEAD = first finger
        fingerIndex = 0;
    } else if (packetType == 6) { // V4_TAIL = second finger 
        fingerIndex = 1;
    } else {
        DEBUG_LOG("ETD0180_MULTITOUCH: Unknown packet type %d\n", packetType);
        return;
    }
    
    // ETD0180 coordinate extraction - TRY V3-style layout for multi-touch
    // Current V4-style gives F1: X=512 (constant), Y jumps chaotically
    // Test V3-style: use bytes [1,2] for X and [4,5] for Y  
    unsigned int x, y;
    if (fingerIndex == 1) {  // Second finger - try V3-style layout
        x = ((packet[1] & 0x0f) << 8) | packet[2];    // V3-style: bytes 1,2
        y = (((packet[4] & 0x0f) << 8) | packet[5]);  // V3-style: bytes 4,5
        DEBUG_LOG("ETD0180_MT_F1_V3: Testing V3-layout X=%d Y=%d\n", x, y);
    } else {  // First finger - keep V4-style (working)
        x = packet[1] | ((packet[3] & 0x0F) << 8);    // V4-style: bytes 1,3  
        y = packet[2] | ((packet[4] & 0x0F) << 8);    // V4-style: bytes 2,4
    }
    
    DEBUG_LOG("ETD0180_MT_F%d: [0x%02x 0x%02x 0x%02x 0x%02x 0x%02x 0x%02x] X=%d Y=%d type=%d\n", 
          fingerIndex, packet[0], packet[1], packet[2], packet[3], packet[4], packet[5], x, y, packetType);
    
    elantechRescale(x, y);
    
    // Finger presence detection
    bool fingerPresent = (packet[0] & 0x30) != 0;  // bits 4-5 indicate finger presence
    
    if (fingerPresent && x > 0 && y > 0 && x < info.x_max && y < info.y_max) {
        // Valid touch detected
        virtualFinger[fingerIndex].touch = true;
        virtualFinger[fingerIndex].prev = virtualFinger[fingerIndex].now;
        // Set coordinates - no swap needed, ETD0180 uses standard orientation
        virtualFinger[fingerIndex].now.x = x;
        virtualFinger[fingerIndex].now.y = info.y_max - y; // Invert Y for macOS
        
        DEBUG_LOG("ETD0180_MT_COORDS: finger=%d x=%d y=%d (inverted_y=%d)\n", 
              fingerIndex, x, y, (int)(info.y_max - y));
        
        // Only first finger controls buttons
        if (fingerIndex == 0) {
            virtualFinger[fingerIndex].button = (leftButton ? 1 : 0) | (rightButton ? 2 : 0);
        } else {
            virtualFinger[fingerIndex].button = 0;
        }
        
        DEBUG_LOG("ETD0180_MT_TOUCH_F%d: Valid touch at X=%d Y=%d (inverted Y=%d)\n", 
              fingerIndex, x, y, (int)virtualFinger[fingerIndex].now.y);
    } else {
        // No finger or invalid coordinates
        virtualFinger[fingerIndex].touch = false;
        virtualFinger[fingerIndex].prev = virtualFinger[fingerIndex].now;
        
        DEBUG_LOG("ETD0180_MT_NO_TOUCH_F%d: finger=%d x=%d y=%d (max X=%d Y=%d)\n", 
              fingerIndex, fingerPresent, x, y, info.x_max, info.y_max);
    }
    
    // CRITICAL FIX: Only send events for the PRIMARY finger (F0)
    // This prevents cursor jumping when F1 packets arrive
    if (fingerIndex == 0) {
        DEBUG_LOG("ETD0180_SENDING_EVENT: Primary finger F0 updated, sending touch data\n");
        sendTouchData();
    } else {
        DEBUG_LOG("ETD0180_SKIP_EVENT: Secondary finger F1 updated, NOT sending event\n");
    }
}

MT2FingerType ApplePS2Elan::GetBestFingerType(int i) {
    switch (i) {
        case 0: return kMT2FingerTypeIndexFinger;
        case 1: return kMT2FingerTypeMiddleFinger;
        case 2: return kMT2FingerTypeRingFinger;
        case 3: return kMT2FingerTypeThumb;
        case 4: return kMT2FingerTypeLittleFinger;

        default:
            break;
    }
    return kMT2FingerTypeIndexFinger;
}


void ApplePS2Elan::sendTouchData() {
    uint64_t timestamp = mach_absolute_time();
    
    // Use mach_absolute_time directly (already in appropriate units for comparison)
    // Note: mach_absolute_time units are platform-dependent but consistent for comparisons

    // Simple button processing - no complex tap-and-hold state machine

    // Ignore input for specified time after keyboard/trackpoint usage
    if (timestamp - keytime < maxaftertyping) {
        return;
    }

    static_assert(VOODOO_INPUT_MAX_TRANSDUCERS >= ETP_MAX_FINGERS, "Trackpad supports too many fingers");

    int transducers_count = 0;
    
    // DEBUG: Log active fingers before sending
    if (IS_ETD0180()) {
        int active_count = 0;
        for (int i = 0; i < ETP_MAX_FINGERS; i++) {
            if (virtualFinger[i].touch) {
                active_count++;
                DEBUG_LOG("ETD0180_SEND: Finger[%d] active at X=%d Y=%d\n", 
                      i, (int)virtualFinger[i].now.x, (int)virtualFinger[i].now.y);
            }
        }
        DEBUG_LOG("ETD0180_SEND: Sending %d active fingers to VoodooInput\n", active_count);
    }
    
    // SMART CLICKPAD LOGIC: Analyze all active fingers ONCE before processing
    bool is_clickpad_pressed = false;
    int total_fingers = 0;
    int navigation_fingers = 0;
    int button_fingers = 0;
    UInt32 button_area_threshold = info.y_max - 400;
    
    if (info.is_buttonpad) {
        // PHASE 1: Initialize button states and analyze all fingers
        leftButton = 0;  // Clear previous button state
        rightButton = 0; // Clear previous button state
        bool left_button_pressed = false;
        bool right_button_pressed = false;
        UInt32 left_right_split = info.x_max / 2;
        
        for (int j = 0; j < ETP_MAX_FINGERS; j++) {
            if (virtualFinger[j].touch) {
                total_fingers++;
                if (virtualFinger[j].now.y < button_area_threshold) {
                    navigation_fingers++;
                } else {
                    button_fingers++;
                    // Check which button area is clicked
                    if (virtualFinger[j].button != 0) {
                        if (virtualFinger[j].now.x < left_right_split) {
                            left_button_pressed = true;
                        } else {
                            right_button_pressed = true;
                        }
                    }
                }
                if (virtualFinger[j].button != 0) {
                    is_clickpad_pressed = true;
                }
            }
        }
        
        // PHASE 2: Set hardware button states
        if (left_button_pressed) {
            leftButton = 1;
            IOLog("ELAN_SMART_CLICKPAD: Setting leftButton=1\n");
        }
        if (right_button_pressed) {
            rightButton = 1; 
            IOLog("ELAN_SMART_CLICKPAD: Setting rightButton=1\n");
        }
        
        if (is_clickpad_pressed) {
            IOLog("ELAN_SMART_CLICKPAD: Total=%d fingers (nav=%d, btn=%d), Left=%d Right=%d, Dimensions: y_max=%d, x_max=%d, threshold=%d, split=%d\n", 
                  total_fingers, navigation_fingers, button_fingers, left_button_pressed, right_button_pressed, info.y_max, info.x_max, button_area_threshold, left_right_split);
        }
    }
    
    for (int i = 0; i < ETP_MAX_FINGERS; i++) {
        const auto &state = virtualFinger[i];
        if (!state.touch) {
            continue;
        }


        auto &transducer = inputEvent.transducers[transducers_count];

        transducer.currentCoordinates = state.now;
        transducer.previousCoordinates = state.prev;
        
        // Convert uint64_t to AbsoluteTime - use reinterpret_cast for compatibility
        transducer.timestamp = *reinterpret_cast<AbsoluteTime*>(&timestamp);

        transducer.isValid = true;
        
        if (info.is_buttonpad && is_clickpad_pressed) {
            // Apply smart clickpad logic based on global analysis
            UInt32 x = state.now.x;
            UInt32 y = state.now.y;
            UInt32 left_right_split = info.x_max / 2;
            
            if (y < button_area_threshold) {  // Navigation area finger
                if (total_fingers == 1) {
                    // SOLO NAVIGATION FINGER: Enable middle click (Force Touch)
                    transducer.isPhysicalButtonDown = false;
                    transducer.supportsPressure = true;
                    transducer.currentCoordinates.pressure = 255;
                    transducer.currentCoordinates.width = 10;
                    IOLog("ELAN_SOLO_NAVIGATION: F%d middle click enabled (Force Touch) at X=%d Y=%d\n", i, x, y);
                } else {
                    // MULTI-FINGER NAVIGATION: Treat as LEFT BUTTON for drag operations
                    transducer.isPhysicalButtonDown = true;
                    transducer.supportsPressure = false;
                    transducer.currentCoordinates.pressure = 0;
                    transducer.currentCoordinates.width = 0;
                    IOLog("ELAN_MULTI_NAVIGATION: F%d treated as LEFT BUTTON for drag (total=%d) at X=%d Y=%d\n", i, total_fingers, x, y);
                }
            } else {  // Button area finger
                // BUTTON AREA: Hardware buttons already set above, just send finger position
                transducer.isPhysicalButtonDown = false;
                transducer.supportsPressure = false;
                transducer.currentCoordinates.pressure = 0;
                transducer.currentCoordinates.width = 0;
                IOLog("ELAN_BUTTON_AREA: F%d at X=%d Y=%d → finger position only (hardware buttons set above)\n", i, x, y);
            }
        } else if (!info.is_buttonpad && state.button != 0) {
            // Traditional trackpad with physical buttons
            transducer.isPhysicalButtonDown = true;
        } else {
            // No button press - MUST clear Force Touch state!
            transducer.isPhysicalButtonDown = false;
            transducer.supportsPressure = false;          // Clear pressure support
            transducer.currentCoordinates.pressure = 0;   // Clear pressure value
            transducer.currentCoordinates.width = 0;      // Clear width
        }
        transducer.isTransducerActive = true;

        transducer.secondaryId = i;
        transducer.fingerType = GetBestFingerType(transducers_count);
        transducer.type = FINGER;

        // it looks like Elan PS2 pressure and width is very inaccurate
        // it is better to leave it that way (except for middle area Force Touch)
        // Don't override if already set to true (middle area Force Touch)
        if (!transducer.supportsPressure) {
            transducer.supportsPressure = false;
        }

        // Force Touch handling for clickpads vs traditional trackpads
        if (info.is_buttonpad) {
            // Clickpad: Force Touch already handled by smart logic above
            // Don't override what the smart clickpad logic decided
        } else {
            // For traditional trackpads with physical buttons: Use force touch if enabled
            if (_forceTouchMode == FORCE_TOUCH_BUTTON && transducer.isPhysicalButtonDown) {
                transducer.supportsPressure = true;
                transducer.isPhysicalButtonDown = false;
                transducer.currentCoordinates.pressure = 255;
                transducer.currentCoordinates.width = 10;
                IOLog("ETD0180_TRADITIONAL_MODE: Using force touch conversion\n");
            }
        }

        transducers_count++;
    }

    // set the thumb to improve 4F pinch and spread gesture and cross-screen dragging
    if (transducers_count >= 4) {
        // simple thumb detection: find the lowest finger touch in the vertical direction
        // note: the origin is top left corner, so lower finger means higher y coordinate
        UInt32 maxY = 0;
        int newThumbIndex = 0;
        int currentThumbIndex = 0;
        for (int i = 0; i < transducers_count; i++) {
            if (inputEvent.transducers[i].currentCoordinates.y > maxY) {
                maxY = inputEvent.transducers[i].currentCoordinates.y;
                newThumbIndex = i;
            }
            if (inputEvent.transducers[i].fingerType == kMT2FingerTypeThumb) {
                currentThumbIndex = i;
            }
        }
        inputEvent.transducers[currentThumbIndex].fingerType = inputEvent.transducers[newThumbIndex].fingerType;
        inputEvent.transducers[newThumbIndex].fingerType = kMT2FingerTypeThumb;
    }

    for (int i = transducers_count; i < VOODOO_INPUT_MAX_TRANSDUCERS; i++) {
        inputEvent.transducers[i].isValid = false;
        inputEvent.transducers[i].isPhysicalButtonDown = false;
        inputEvent.transducers[i].isTransducerActive = false;
    }

    inputEvent.contact_count = transducers_count;
    
    // Convert uint64_t to AbsoluteTime - use reinterpret_cast for compatibility
    inputEvent.timestamp = *reinterpret_cast<AbsoluteTime*>(&timestamp);

    if (voodooInputInstance) {
        super::messageClient(kIOMessageVoodooInputMessage, voodooInputInstance, &inputEvent, sizeof(VoodooInputEvent));
        IOLog("ELAN_VOODINPUT_SUCCESS: Event sent to voodooInputInstance with %d contacts\n", transducers_count);
    } else {
        IOLog("ELAN_VOODINPUT_ERROR: voodooInputInstance is NULL - cannot send events!\n");
    }

    // Process hardware buttons with middle button simulation
    // Note: Now processing buttons for both traditional trackpads AND buttonpads/clickpads
    {
        // Raw hardware buttons
        UInt32 rawButtons = (leftButton ? 1 : 0) | (rightButton ? 2 : 0);
        
        // Apply middle button state machine
        UInt32 processedButtons = middleButton(rawButtons, timestamp, fromMouse);
        lastbuttons = processedButtons;
        
        // Always send button events when buttons are pressed, regardless of finger count
        bool send = false;
        if (transducers_count == 0) {
            // Always send when no fingers (trackpoint mode)
            send = true;
        } else {
            // Send when button state changed (trackpad mode)
            if (lastLeftButton != leftButton || lastRightButton != rightButton) {
                send = true;
            }
            // ALSO send when buttons are currently pressed (for clickpad button area)
            if (leftButton || rightButton) {
                send = true;
            }
        }
        
        if (send) {
            // Convert uint64_t to AbsoluteTime - use reinterpret_cast for compatibility
            trackpointReport.timestamp = *reinterpret_cast<AbsoluteTime*>(&timestamp);
            trackpointReport.buttons = processedButtons;
            trackpointReport.dx = trackpointReport.dy = 0;
            super::messageClient(kIOMessageVoodooTrackpointMessage, voodooInputInstance,
                                 &trackpointReport, sizeof(trackpointReport));
            IOLog("ELAN_BUTTON_SENT: rawButtons=%d processedButtons=%d (L=%d R=%d)\n", rawButtons, processedButtons, leftButton, rightButton);
        }

        lastLeftButton = leftButton;
        lastRightButton = rightButton;
    }
}

PS2InterruptResult ApplePS2Elan::interruptOccurred(UInt8 data) {
    UInt8 *packet = _ringBuffer.head();
    packet[_packetByteCount++] = data;

    if (_packetByteCount == _packetLength) {
        _ringBuffer.advanceHead(_packetLength);
        _packetByteCount = 0;
        return kPS2IR_packetReady;
    }

    return kPS2IR_packetBuffering;
}

void ApplePS2Elan::packetReady() {
    INTERRUPT_LOG("VoodooPS2Elan: packet ready occurred\n");
    // empty the ring buffer, dispatching each packet...
    while (_ringBuffer.count() >= _packetLength) {
        if (ignoreall) {
            _ringBuffer.advanceTail(_packetLength);
            continue;
        }

        int packetType;
        switch (info.hw_version) {
            case 1:
                if (info.paritycheck && !elantechPacketCheckV1()) {
                    // ignore invalid packet
                    INTERRUPT_LOG("VoodooPS2Elan: invalid packet received\n");
                    break;
                }

                INTERRUPT_LOG("VoodooPS2Elan: Handling absolute mode\n");
                elantechReportAbsoluteV1();
                break;

            case 2:
                if (elantechDebounceCheckV2()) {
                    // ignore debounce
                    break;
                }

                if (info.paritycheck && !elantechPacketCheckV2()) {
                    // ignore invalid packet
                    INTERRUPT_LOG("VoodooPS2Elan: invalid packet received\n");
                    break;
                }

                INTERRUPT_LOG("VoodooPS2Elan: Handling absolute mode\n");
                elantechReportAbsoluteV2();
                break;

            case 3:
                packetType = elantechPacketCheckV3();
                INTERRUPT_LOG("VoodooPS2Elan: Packet Type %d\n", packetType);

                switch (packetType) {
                    case PACKET_UNKNOWN:
                        INTERRUPT_LOG("VoodooPS2Elan: invalid packet received\n");
                        break;

                    case PACKET_DEBOUNCE:
                        // ignore debounce
                        break;

                    case PACKET_TRACKPOINT:
                        INTERRUPT_LOG("VoodooPS2Elan: Handling trackpoint packet\n");
                        elantechReportTrackpoint();
                        break;

                    default:
                        INTERRUPT_LOG("VoodooPS2Elan: Handling absolute mode\n");
                        elantechReportAbsoluteV3(packetType);
                        break;
                }
                break;

            case 4:
                // Normal V4 handling
                {
                    UInt8 *packet = _ringBuffer.tail();
                    DEBUG_LOG("VoodooPS2Elan: Processing V4 packet [%02x %02x %02x %02x %02x %02x]\n", 
                              packet[0], packet[1], packet[2], packet[3], packet[4], packet[5]);
                }
                packetType = elantechPacketCheckV4();
                INTERRUPT_LOG("VoodooPS2Elan: Packet Type %d\n", packetType);

                switch (packetType) {
                    case PACKET_UNKNOWN:
                        INTERRUPT_LOG("VoodooPS2Elan: invalid packet received\n");
                        break;

                    case PACKET_TRACKPOINT:
                        INTERRUPT_LOG("VoodooPS2Elan: Handling trackpoint packet\n");
                        elantechReportTrackpoint();
                        break;


                    default:
                        INTERRUPT_LOG("VoodooPS2Elan: Handling absolute mode\n");
                        elantechReportAbsoluteV4(packetType);
                        break;
                }
                break;

            default:
                INTERRUPT_LOG("VoodooPS2Elan: invalid packet received\n");
        }

        _ringBuffer.advanceTail(_packetLength);
    }
}

void ApplePS2Elan::resetMouse() {
    UInt8 params[2];
    ps2_command<2>(params, kDP_Reset);

    if (params[0] != 0xaa && params[1] != 0x00) {
        DEBUG_LOG("VoodooPS2Elan: failed resetting.\n");
    }
}

void ApplePS2Elan::setTouchPadEnable(bool enable) {
    ps2_command<0>(NULL, enable ? kDP_Enable : kDP_SetDefaultsAndDisable);
}

// Simple Button Timer Handler (adapted from VoodooPS2Mouse)
void ApplePS2Elan::onButtonTimer(void) {
    uint64_t now_abs;
    clock_get_uptime(&now_abs);
    
    middleButton(lastbuttons, now_abs, fromTimer);
}

// Check if any finger is actively touching the trackpad
bool ApplePS2Elan::isAnyFingerActive(void) {
    for (int i = 0; i < 5; i++) {
        if (virtualFinger[i].touch) {
            // Log any active finger position for debugging  
            DEBUG_LOG("VoodooPS2Elan: ACTIVE_FINGER_DETECTED: F%d at X=%d Y=%d (trackpad_max=%dx%d)\n", 
                     i, (int)virtualFinger[i].now.x, (int)virtualFinger[i].now.y, info.x_max, info.y_max);
            return true;  // ANY active finger should trigger prevention logic
        }
    }
    return false;
}

// Check if multiple fingers are active with one in navigation area
bool ApplePS2Elan::isMultiFingerWithNavigation(void) {
    const UInt32 button_area_height = 100;
    if (info.y_max <= button_area_height) return false;
    UInt32 nav_threshold = info.y_max - button_area_height;
    
    int active_fingers = 0;
    bool has_nav_finger = false;
    
    for (int i = 0; i < ETP_MAX_FINGERS; i++) {
        if (virtualFinger[i].touch) {
            active_fingers++;
            if (virtualFinger[i].now.y < nav_threshold) {
                has_nav_finger = true;
                IOLog("VoodooPS2Elan: NAV_FINGER_F%d: y=%d < %d (nav area)\n",
                          i, (int)virtualFinger[i].now.y, nav_threshold);
            }
        }
    }
    
    IOLog("VoodooPS2Elan: FINGER_COUNT: %d active, nav_finger=%s\n", 
              active_fingers, has_nav_finger ? "YES" : "NO");
    
    // Prevention nur wenn: MEHR als 1 Finger UND mindestens einer im Nav-Bereich
    return (active_fingers > 1) && has_nav_finger;
}

// Simple Middle Button State Machine (adapted from VoodooPS2Mouse)
UInt32 ApplePS2Elan::middleButton(UInt32 buttons, uint64_t now_abs, MBComingFrom from) {
    // Skip fake middle button logic for buttonpads - they have smart clickpad logic
    if (!_fakemiddlebutton || info.is_buttonpad)
        return buttons;
    
    // Log any button activity for debugging
    if (buttons != 0) {
        DEBUG_LOG("VoodooPS2Elan: RAW_BUTTON_INPUT: buttons=0x%x (L=%s R=%s M=%s)\n", 
                 buttons, 
                 (buttons & 0x1) ? "ON" : "OFF", 
                 (buttons & 0x2) ? "ON" : "OFF", 
                 (buttons & 0x4) ? "ON" : "OFF");
    }
    
    // BUGFIX: Prevent middle click when finger is in button area
    // If finger is navigating in button area and physical button is pressed,
    // prioritize physical button over finger touch to avoid unintended middle click
    if (buttons == 0x3) {
        IOLog("VoodooPS2Elan: MIDDLE_CLICK_DETECTED: Raw buttons=0x%x (L=%s R=%s)\n", 
                 buttons, (buttons & 0x1) ? "YES" : "NO", (buttons & 0x2) ? "YES" : "NO");
        
        if (isMultiFingerWithNavigation()) {
            // Multi-finger scenario: Navigation finger + button finger detected
            // Prioritize left physical button (most common case)
            IOLog("VoodooPS2Elan: MIDDLE_CLICK_PREVENTION: Multi-finger with navigation area detected - converting to left click\n");
            IOLog("VoodooPS2Elan: BUTTON_OVERRIDE: Changed buttons from 0x%x to 0x1 (middle->left)\n", buttons);
            return 0x1;  // Return only left button, ignore finger touch
        } else {
            IOLog("VoodooPS2Elan: MIDDLE_CLICK_ALLOWED: Single finger or no navigation area finger - allowing middle click\n");
        }
    }
    
    // cancel timer if we see input before timeout has fired, but after expired
    bool timeout = false;
    uint64_t now_ns;
    absolutetime_to_nanoseconds(now_abs, &now_ns);
    if (fromTimer == from || now_ns - _buttontime > _maxmiddleclicktime)
        timeout = true;
    
    // Simple state machine to simulate middle buttons with two buttons pressed together
    switch (_mbuttonstate) {
        case STATE_NOBUTTONS:
            if (buttons & 0x4)
                _mbuttonstate = STATE_NOOP;
            else if (0x3 == buttons)
                _mbuttonstate = STATE_MIDDLE;
            else if (0x0 != buttons) {
                // only single button, so delay this for a bit
                _pendingbuttons = buttons;
                _buttontime = now_ns;
                if (_buttonTimer) {
                    _buttonTimer->setTimeout(_maxmiddleclicktime);
                }
                _mbuttonstate = STATE_WAIT4TWO;
            }
            break;
            
        case STATE_WAIT4TWO:
            if (!timeout && 0x3 == buttons) {
                _pendingbuttons = 0;
                if (_buttonTimer) {
                    _buttonTimer->cancelTimeout();
                }
                _mbuttonstate = STATE_MIDDLE;
            } else if (timeout || buttons != _pendingbuttons) {
                _pendingbuttons = 0;
                if (_buttonTimer) {
                    _buttonTimer->cancelTimeout();
                }
                if (0x0 == buttons)
                    _mbuttonstate = STATE_NOBUTTONS;
                else
                    _mbuttonstate = STATE_NOOP;
            }
            break;
            
        case STATE_MIDDLE:
            if (0x0 == buttons)
                _mbuttonstate = STATE_NOBUTTONS;
            else if (0x3 != (buttons & 0x3)) {
                // only single button, so delay to see if we get to none
                _pendingbuttons = buttons;
                _buttontime = now_ns;
                if (_buttonTimer) {
                    _buttonTimer->setTimeout(_maxmiddleclicktime);
                }
                _mbuttonstate = STATE_WAIT4NONE;
            }
            break;
            
        case STATE_WAIT4NONE:
            if (!timeout && 0x0 == buttons) {
                _pendingbuttons = 0;
                if (_buttonTimer) {
                    _buttonTimer->cancelTimeout();
                }
                _mbuttonstate = STATE_NOBUTTONS;
            } else if (timeout || buttons != _pendingbuttons) {
                _pendingbuttons = 0;
                if (_buttonTimer) {
                    _buttonTimer->cancelTimeout();
                }
                if (0x0 == buttons)
                    _mbuttonstate = STATE_NOBUTTONS;
                else
                    _mbuttonstate = STATE_NOOP;
            }
            break;
            
        case STATE_NOOP:
            if (0x0 == buttons)
                _mbuttonstate = STATE_NOBUTTONS;
            break;
    }
    
    // modify buttons after new state set
    switch (_mbuttonstate) {
        case STATE_MIDDLE:
            buttons = 0x4;  // Middle button
            break;
            
        case STATE_WAIT4NONE:
        case STATE_WAIT4TWO:
            buttons &= ~0x3;  // Clear left and right buttons
            break;
            
        case STATE_NOBUTTONS:
        case STATE_NOOP:
            break;
    }
    
    return buttons;
}
