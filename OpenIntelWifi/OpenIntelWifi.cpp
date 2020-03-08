/* add your code here */
#include "IONetworkInterface.h"
#include "IONetworkController.h"
#include "OpenIntelWifi.hpp"

OSDefineMetaClassAndStructors(itlwm, IO80211Controller);
#define super IO80211Controller
OSDefineMetaClassAndStructors(CTimeout, OSObject)

IOWorkLoop *_fWorkloop;

const char *fake_ssid = "UPC5424297";
const uint8_t fake_bssid[] = { 0x64, 0x7C, 0x34, 0x5C, 0x1C, 0x40 };
const char *fake_hw_version = "Hardware 1.0";
const char *fake_drv_version = "Driver 1.0";
const char *fake_country_code = "CZ";

const apple80211_channel fake_channel = {
    .version = APPLE80211_VERSION,
    .channel = 1,
    .flags = APPLE80211_C_FLAG_2GHZ | APPLE80211_C_FLAG_20MHZ | APPLE80211_C_FLAG_ACTIVE
};

const char beacon_ie[] = "\x00\x0a\x55" \
"\x50\x43\x35\x34\x32\x34\x32\x39\x37\x01\x08\x82\x84\x8b\x96\x0c" \
"\x12\x18\x24\x03\x01\x01\x05\x04\x00\x01\x00\x00\x07\x06\x43\x5a" \
"\x20\x01\x0d\x14\x2a\x01\x04\x32\x04\x30\x48\x60\x6c\x2d\x1a\xad" \
"\x01\x1b\xff\xff\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00" \
"\x00\x00\x00\x04\x06\xe6\xe7\x0d\x00\x3d\x16\x01\x00\x17\x00\x00" \
"\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00" \
"\x00\x4a\x0e\x14\x00\x0a\x00\x2c\x01\xc8\x00\x14\x00\x05\x00\x19" \
"\x00\x7f\x01\x01\xdd\x18\x00\x50\xf2\x02\x01\x01\x80\x00\x03\xa4" \
"\x00\x00\x27\xa4\x00\x00\x42\x43\x5e\x00\x62\x32\x2f\x00\xdd\x09" \
"\x00\x03\x7f\x01\x01\x00\x00\xff\x7f\x30\x18\x01\x00\x00\x0f\xac" \
"\x02\x02\x00\x00\x0f\xac\x04\x00\x0f\xac\x02\x01\x00\x00\x0f\xac" \
"\x02\x00\x00\xdd\x1a\x00\x50\xf2\x01\x01\x00\x00\x50\xf2\x02\x02" \
"\x00\x00\x50\xf2\x04\x00\x50\xf2\x02\x01\x00\x00\x50\xf2\x02\xdd" \
"\x22\x00\x50\xf2\x04\x10\x4a\x00\x01\x10\x10\x44\x00\x01\x02\x10" \
"\x57\x00\x01\x01\x10\x3c\x00\x01\x01\x10\x49\x00\x06\x00\x37\x2a" \
"\x00\x01\x20";

bool itlwm::init(OSDictionary* parameters) {
    XYLog("Init");
    if (!super::init(parameters)) {
        IOLog("Failed to call IO80211Controller::init!");
        return false;
    }
    fwLoadLock = IOLockAlloc();
    return true;
}

void itlwm::free() {
    XYLog("Free");
    
    ReleaseAll();
    super::free();
}

IOService* itlwm::probe(IOService *provider, SInt32 *score) {
    XYLog("probing");
    
    super::probe(provider, score);
    
    fPciDevice = OSDynamicCast(IOPCIDevice, provider);
    if (!fPciDevice) {
        XYLog("not PCI device");
        fPciDevice = NULL;
        return NULL;
    }
    IOPCIDevice* device = OSDynamicCast(IOPCIDevice, provider);
    if (!device) {
        return NULL;
    }
    if (!iwm_match(device)) {
        return NULL;
    }
    
    fPciDevice->retain();
    
    return this;
}

bool itlwm::createWorkLoop() {
    if(!fWorkloop) {
        fWorkloop = IO80211WorkLoop::workLoop();
    }
    
    return (fWorkloop != NULL);
}

IOWorkLoop* itlwm::getWorkLoop() const {
    return fWorkloop;
}

bool itlwm::start(IOService* provider) {
    XYLog("Start");
    ifnet *ifp;
    if (!super::start(provider)) {
        XYLog("Failed to call IO80211Controller::start!");
        ReleaseAll();
        return false;
    }
    IOPCIDevice* device = OSDynamicCast(IOPCIDevice, provider);
    if (!device) {
        return false;
    }
    device->setBusMasterEnable(true);
    device->setIOEnable(true);
    device->setMemoryEnable(true);
    device->configWrite8(0x41, 0);
    fWorkloop = (IO80211WorkLoop *)getWorkLoop();
    if (!fWorkloop) {
        XYLog("Failed to get workloop!");
        ReleaseAll();
        return false;
    }
    _fWorkloop = fWorkloop;
    
    fCommandGate = IOCommandGate::commandGate(this);
    if (!fCommandGate) {
        XYLog("Failed to create command gate!");
        ReleaseAll();
        return false;
    }
    
    if (fWorkloop->addEventSource(fCommandGate) != kIOReturnSuccess) {
        XYLog("Failed to register command gate event source!");
        ReleaseAll();
        return false;
    }
    
    fCommandGate->enable();
    
    mediumDict = OSDictionary::withCapacity(MEDIUM_TYPE_INVALID + 1);
    addMediumType(kIOMediumIEEE80211None,  0,  MEDIUM_TYPE_NONE);
    addMediumType(kIOMediumIEEE80211Auto,  0,  MEDIUM_TYPE_AUTO);
    addMediumType(kIOMediumIEEE80211DS1,   1000000, MEDIUM_TYPE_1MBIT);
    addMediumType(kIOMediumIEEE80211DS2,   2000000, MEDIUM_TYPE_2MBIT);
    addMediumType(kIOMediumIEEE80211DS5,   5500000, MEDIUM_TYPE_5MBIT);
    addMediumType(kIOMediumIEEE80211DS11, 11000000, MEDIUM_TYPE_11MBIT);
    addMediumType(kIOMediumIEEE80211,     54000000, MEDIUM_TYPE_54MBIT, "OFDM54");
    //addMediumType(kIOMediumIEEE80211OptionAdhoc, 0, MEDIUM_TYPE_ADHOC,"ADHOC");
    
    if (!publishMediumDictionary(mediumDict)) {
        XYLog("Failed to publish medium dictionary!");
        ReleaseAll();
        return false;
    }
    
    if (!setCurrentMedium(mediumTable[MEDIUM_TYPE_AUTO])) {
        XYLog("Failed to set current medium!");
        ReleaseAll();
        return false;
    }
    if (!setSelectedMedium(mediumTable[MEDIUM_TYPE_AUTO])) {
        XYLog("Failed to set selected medium!");
        ReleaseAll();
        return false;
    }
    pci.workloop = _fWorkloop;
    pci.pa_tag = device;
//    if (!iwm_attach(&com, &pci)) {
//        return false;
//    }
    ifp = &com.sc_ic.ic_ac.ac_if;
    ifp->iface = fInterface;
//    iwm_init(ifp);
    if (!attachInterface((IONetworkInterface **)&com.sc_ic.ic_ac.ac_if.iface, true)) {
        XYLog("attach to interface fail\n");
        ReleaseAll();
        return false;
    }
    
    fInterface->registerService();
    registerService();

    return true;
}

IOReturn itlwm::enable(IONetworkInterface* iface) {
    XYLog("enable");
    IOMediumType mediumType = kIOMediumIEEE80211Auto;
    IONetworkMedium *medium = IONetworkMedium::getMediumWithType(mediumDict, mediumType);
    setLinkStatus(kIONetworkLinkActive | kIONetworkLinkValid, medium);
    
    if(fInterface) {
        fInterface->postMessage(1);
    }
    
    return kIOReturnSuccess;
}

IOReturn itlwm::disable(IONetworkInterface* iface) {
    XYLog("disable");
    return kIOReturnSuccess;
}

bool itlwm::addMediumType(UInt32 type, UInt32 speed, UInt32 code, char* name) {
    bool ret = false;
    
    IONetworkMedium* medium = IONetworkMedium::medium(type, speed, 0, code, name);
    if (medium) {
        ret = IONetworkMedium::addMedium(mediumDict, medium);
        if (ret)
            mediumTable[code] = medium;
        medium->release();
    }
    return ret;
}


void itlwm::stop(IOService* provider) {
    if (fCommandGate) {
        IOLog("stop: Command gate alive. Disabling it.");
        if (fCommandGate->isEnabled()) {
            fCommandGate->disable();
        }
        IOLog("stop: Done disabling command gate");
        if (fWorkloop) {
            IOLog("stop: Workloop alive. Removing command gate");
            fWorkloop->removeEventSource(fCommandGate);
            fCommandGate->release();
        }
    }
    
    if (fInterface) {
        IOLog("stop: Detaching interface");
        fInterface->detach(this);
        fInterface->stop(this);
        detachInterface(fInterface, true);
        fInterface->release();
    }
    
    super::stop(provider);
}

IOReturn itlwm::getHardwareAddress(IOEthernetAddress* addrP) {
    XYLog("%s\n", __func__);
    if (IEEE80211_ADDR_EQ(etheranyaddr, com.sc_ic.ic_myaddr)) {
        return kIOReturnError;
    } else {
        IEEE80211_ADDR_COPY(addrP, com.sc_ic.ic_myaddr);
        return kIOReturnSuccess;
    }
}

IOReturn itlwm::getHardwareAddressForInterface(IO80211Interface* netif,
                                                           IOEthernetAddress* addr) {
    return getHardwareAddress(addr);
}

SInt32 itlwm::apple80211Request(unsigned int request_type,
                                            int request_number,
                                            IO80211Interface* interface,
                                            void* data) {
    if (request_type != SIOCGA80211 && request_type != SIOCSA80211) {
        IOLog("Invalid IOCTL request type: %u", request_type);
        IOLog("Expected either %lu or %lu", SIOCGA80211, SIOCSA80211);
        return kIOReturnError;
    }

    IOReturn ret = 0;
    
    bool isGet = (request_type == SIOCGA80211);
    
#define IOCTL(REQ_TYPE, REQ, DATA_TYPE) \
if (REQ_TYPE == SIOCGA80211) { \
ret = get##REQ(interface, (struct DATA_TYPE* )data); \
} else { \
ret = set##REQ(interface, (struct DATA_TYPE* )data); \
}
    
#define IOCTL_GET(REQ_TYPE, REQ, DATA_TYPE) \
if (REQ_TYPE == SIOCGA80211) { \
    ret = get##REQ(interface, (struct DATA_TYPE* )data); \
}
#define IOCTL_SET(REQ_TYPE, REQ, DATA_TYPE) \
if (REQ_TYPE == SIOCSA80211) { \
    ret = set##REQ(interface, (struct DATA_TYPE* )data); \
}
    
    XYLog("IOCTL %s(%d) %s",
          isGet ? "get" : "set",
          request_number,
          IOCTL_NAMES[request_number]);
    
    switch (request_number) {
        case APPLE80211_IOC_SSID: // 1
            IOCTL(request_type, SSID, apple80211_ssid_data);
            break;
        case APPLE80211_IOC_AUTH_TYPE: // 2
            IOCTL_GET(request_type, AUTH_TYPE, apple80211_authtype_data);
            break;
        case APPLE80211_IOC_CHANNEL: // 4
            IOCTL_GET(request_type, CHANNEL, apple80211_channel_data);
            break;
        case APPLE80211_IOC_TXPOWER: // 7
            IOCTL_GET(request_type, TXPOWER, apple80211_txpower_data);
            break;
        case APPLE80211_IOC_RATE: // 8
            IOCTL_GET(request_type, RATE, apple80211_rate_data);
            break;
        case APPLE80211_IOC_BSSID: // 9
            IOCTL_GET(request_type, BSSID, apple80211_bssid_data);
            break;
        case APPLE80211_IOC_SCAN_REQ: // 10
            IOCTL_SET(request_type, SCAN_REQ, apple80211_scan_data);
            break;
        case APPLE80211_IOC_SCAN_RESULT: // 11
            IOCTL_GET(request_type, SCAN_RESULT, apple80211_scan_result*);
            break;
        case APPLE80211_IOC_CARD_CAPABILITIES: // 12
            IOCTL_GET(request_type, CARD_CAPABILITIES, apple80211_capability_data);
            break;
        case APPLE80211_IOC_STATE: // 13
            IOCTL_GET(request_type, STATE, apple80211_state_data);
            break;
        case APPLE80211_IOC_PHY_MODE: // 14
            IOCTL_GET(request_type, PHY_MODE, apple80211_phymode_data);
            break;
        case APPLE80211_IOC_OP_MODE: // 15
            IOCTL_GET(request_type, OP_MODE, apple80211_opmode_data);
            break;
        case APPLE80211_IOC_RSSI: // 16
            IOCTL_GET(request_type, RSSI, apple80211_rssi_data);
            break;
        case APPLE80211_IOC_NOISE: // 17
            IOCTL_GET(request_type, NOISE, apple80211_noise_data);
            break;
        case APPLE80211_IOC_INT_MIT: // 18
            IOCTL_GET(request_type, INT_MIT, apple80211_intmit_data);
            break;
        case APPLE80211_IOC_POWER: // 19
            IOCTL(request_type, POWER, apple80211_power_data);
            break;
        case APPLE80211_IOC_ASSOCIATE: // 20
            IOCTL_SET(request_type, ASSOCIATE, apple80211_assoc_data);
            break;
        case APPLE80211_IOC_SUPPORTED_CHANNELS: // 27
            IOCTL_GET(request_type, SUPPORTED_CHANNELS, apple80211_sup_channel_data);
            break;
        case APPLE80211_IOC_LOCALE: // 28
            IOCTL_GET(request_type, LOCALE, apple80211_locale_data);
            break;
        case APPLE80211_IOC_TX_ANTENNA: // 37
            IOCTL_GET(request_type, TX_ANTENNA, apple80211_antenna_data);
            break;
        case APPLE80211_IOC_ANTENNA_DIVERSITY: // 39
            IOCTL_GET(request_type, ANTENNA_DIVERSITY, apple80211_antenna_data);
            break;
        case APPLE80211_IOC_DRIVER_VERSION: // 43
            IOCTL_GET(request_type, DRIVER_VERSION, apple80211_version_data);
            break;
        case APPLE80211_IOC_HARDWARE_VERSION: // 44
            IOCTL_GET(request_type, HARDWARE_VERSION, apple80211_version_data);
            break;
        case APPLE80211_IOC_COUNTRY_CODE: // 51
            IOCTL_GET(request_type, COUNTRY_CODE, apple80211_country_code_data);
            break;
        case APPLE80211_IOC_RADIO_INFO:
            IOCTL_GET(request_type, RADIO_INFO, apple80211_radio_info_data);
            break;
        case APPLE80211_IOC_MCS: // 57
            IOCTL_GET(request_type, MCS, apple80211_mcs_data);
            break;
        case APPLE80211_IOC_WOW_PARAMETERS: // 69
            break;
        case APPLE80211_IOC_ROAM_THRESH:
            IOCTL_GET(request_type, ROAM_THRESH, apple80211_roam_threshold_data);
            break;
        case APPLE80211_IOC_TX_CHAIN_POWER: // 108
            break;
        case APPLE80211_IOC_THERMAL_THROTTLING: // 111
            break;
        default:
            XYLog("unhandled ioctl %s %d", IOCTL_NAMES[request_number], request_number);
            break;
    }
#undef IOCTL
    
    return ret;
}

bool itlwm::configureInterface(IONetworkInterface *netif) {
    XYLog("Configure interface");
    if (!super::configureInterface(netif)) {
        return false;
    }

    return true;
}

IO80211Interface* itlwm::getNetworkInterface() {
    return fInterface;
}

UInt32 itlwm::outputPacket(mbuf_t m, void* param) {
    freePacket(m);
    return kIOReturnSuccess;
}

IOReturn itlwm::getMaxPacketSize( UInt32* maxSize ) const {
    *maxSize = 1500;
    return kIOReturnSuccess;
}

IOReturn itlwm::setPromiscuousMode(IOEnetPromiscuousMode mode) {
    return kIOReturnSuccess;
}

IOReturn itlwm::setMulticastMode(IOEnetMulticastMode mode) {
    return kIOReturnSuccess;
}

IOReturn itlwm::setMulticastList(IOEthernetAddress* addr, UInt32 len) {
    return kIOReturnSuccess;
}

SInt32 itlwm::monitorModeSetEnabled(IO80211Interface* interface,
                                                bool enabled,
                                                UInt32 dlt) {
    return kIOReturnSuccess;
}

const OSString* itlwm::newVendorString() const {
    return OSString::withCString("black_wizard");
}

const OSString* itlwm::newModelString() const {
    return OSString::withCString("BlackControl80211");
}

const OSString* itlwm::newRevisionString() const {
    return OSString::withCString("1.0");
}

//
// MARK: 1 - SSID
//

IOReturn itlwm::getSSID(IO80211Interface *interface,
                                    struct apple80211_ssid_data *sd) {
    
    bzero(sd, sizeof(*sd));
    sd->version = APPLE80211_VERSION;
    strncpy((char*)sd->ssid_bytes, fake_ssid, sizeof(sd->ssid_bytes));
    sd->ssid_len = (uint32_t)strlen(fake_ssid);

    return kIOReturnSuccess;
}

IOReturn itlwm::setSSID(IO80211Interface *interface,
                                    struct apple80211_ssid_data *sd) {
    
    fInterface->postMessage(APPLE80211_M_SSID_CHANGED);
    return kIOReturnSuccess;
}

//
// MARK: 2 - AUTH_TYPE
//

IOReturn itlwm::getAUTH_TYPE(IO80211Interface *interface,
                                         struct apple80211_authtype_data *ad) {
    ad->version = APPLE80211_VERSION;
    ad->authtype_lower = APPLE80211_AUTHTYPE_OPEN;
    ad->authtype_upper = APPLE80211_AUTHTYPE_NONE;
    return kIOReturnSuccess;
}

//
// MARK: 4 - CHANNEL
//

IOReturn itlwm::getCHANNEL(IO80211Interface *interface,
                                       struct apple80211_channel_data *cd) {
    //return kIOReturnError;
    
    memset(cd, 0, sizeof(apple80211_channel_data));
    //bzero(cd, sizeof(apple80211_channel_data));
    
    cd->version = APPLE80211_VERSION;
    cd->channel = fake_channel;
    return kIOReturnSuccess;
}

//
// MARK: 7 - TXPOWER
//

IOReturn itlwm::getTXPOWER(IO80211Interface *interface,
                                       struct apple80211_txpower_data *txd) {

    txd->version = APPLE80211_VERSION;
    txd->txpower = 100;
    txd->txpower_unit = APPLE80211_UNIT_PERCENT;
    return kIOReturnSuccess;
}

//
// MARK: 8 - RATE
//

IOReturn itlwm::getRATE(IO80211Interface *interface, struct apple80211_rate_data *rd) {
    rd->version = APPLE80211_VERSION;
    rd->num_radios = 1;
    rd->rate[0] = 54;
    return kIOReturnSuccess;
}

//
// MARK: 9 - BSSID
//

IOReturn itlwm::getBSSID(IO80211Interface *interface,
                                     struct apple80211_bssid_data *bd) {
    
    bzero(bd, sizeof(*bd));
    
    bd->version = APPLE80211_VERSION;
    memcpy(bd->bssid.octet, fake_bssid, sizeof(fake_bssid));
    return kIOReturnSuccess;
}

static IOReturn scanAction(OSObject *target, void *arg0, void *arg1, void *arg2, void *arg3) {
    IOSleep(2000);
    IO80211Interface *iface = (IO80211Interface *)arg0;
    iface->postMessage(APPLE80211_M_SCAN_DONE);
    return kIOReturnSuccess;
}

//
// MARK: 10 - SCAN_REQ
//
IOReturn itlwm::setSCAN_REQ(IO80211Interface *interface,
                                        struct apple80211_scan_data *sd) {
//    if (dev->state() == APPLE80211_S_SCAN) {
//        return kIOReturnBusy;
//    }
//    dev->setState(APPLE80211_S_SCAN);
    XYLog("Scan requested. Type: %u\n"
          "BSS Type: %u\n"
          "PHY Mode: %u\n"
          "Dwell time: %u\n"
          "Rest time: %u\n"
          "Num channels: %u\n",
          sd->scan_type,
          sd->bss_type,
          sd->phy_mode,
          sd->dwell_time,
          sd->rest_time,
          sd->num_channels);
    
//    if (interface) {
//        dev->setPublished(false);
//        fCommandGate->runAction(scanAction, interface, dev);
//    }
    
    return kIOReturnSuccess;
}

//
// MARK: 11 - SCAN_RESULT
//
IOReturn itlwm::getSCAN_RESULT(IO80211Interface *interface,
                                           struct apple80211_scan_result **sr) {
//    if () {
//        dev->setState(APPLE80211_S_INIT);
//        return 0xe0820446;
//    }
    
    struct apple80211_scan_result* result =
        (struct apple80211_scan_result*)IOMalloc(sizeof(struct apple80211_scan_result));
    
    
    
    bzero(result, sizeof(*result));
    result->version = APPLE80211_VERSION;
    
    result->asr_channel = fake_channel;
    
    result->asr_noise = -101;
//    result->asr_snr = 60;
    result->asr_rssi = -73;
    result->asr_beacon_int = 100;
    
    result->asr_cap = 0x411;
    
    result->asr_age = 0;
    
    memcpy(result->asr_bssid, fake_bssid, sizeof(fake_bssid));
    
    result->asr_nrates = 1;
    result->asr_rates[0] = 54;
    
    strncpy((char*)result->asr_ssid, fake_ssid, sizeof(result->asr_ssid));
    result->asr_ssid_len = strlen(fake_ssid);
    
    result->asr_ie_len = 246;
    result->asr_ie_data = IOMalloc(result->asr_ie_len);
    memcpy(result->asr_ie_data, beacon_ie, result->asr_ie_len);

    *sr = result;
    
//    dev->setPublished(true);
    
    return kIOReturnSuccess;
}

//
// MARK: 12 - CARD_CAPABILITIES
//

IOReturn itlwm::getCARD_CAPABILITIES(IO80211Interface *interface,
                                                 struct apple80211_capability_data *cd) {
    cd->version = APPLE80211_VERSION;
    cd->capabilities[0] = 0xab;
    cd->capabilities[1] = 0x7e;
    return kIOReturnSuccess;
}

//
// MARK: 13 - STATE
//

IOReturn itlwm::getSTATE(IO80211Interface *interface,
                                     struct apple80211_state_data *sd) {
    sd->version = APPLE80211_VERSION;
//    sd->state = dev->state();
    return kIOReturnSuccess;
}

IOReturn itlwm::setSTATE(IO80211Interface *interface,
                                     struct apple80211_state_data *sd) {
    XYLog("Setting state: %u", sd->state);
//    dev->setState(sd->state);
    return kIOReturnSuccess;
}

//
// MARK: 14 - PHY_MODE
//

IOReturn itlwm::getPHY_MODE(IO80211Interface *interface,
                                        struct apple80211_phymode_data *pd) {
    pd->version = APPLE80211_VERSION;
    pd->phy_mode = APPLE80211_MODE_11A
                 | APPLE80211_MODE_11B
                 | APPLE80211_MODE_11G;
    pd->active_phy_mode = APPLE80211_MODE_AUTO;
    return kIOReturnSuccess;
}

//
// MARK: 15 - OP_MODE
//

IOReturn itlwm::getOP_MODE(IO80211Interface *interface,
                                       struct apple80211_opmode_data *od) {
    od->version = APPLE80211_VERSION;
    od->op_mode = APPLE80211_M_STA;
    return kIOReturnSuccess;
}

//
// MARK: 16 - RSSI
//

IOReturn itlwm::getRSSI(IO80211Interface *interface,
                                    struct apple80211_rssi_data *rd) {
    
    bzero(rd, sizeof(*rd));
    rd->version = APPLE80211_VERSION;
    rd->num_radios = 1;
    rd->rssi[0] = -42;
    rd->aggregate_rssi = -42;
    rd->rssi_unit = APPLE80211_UNIT_DBM;
    return kIOReturnSuccess;
}

//
// MARK: 17 - NOISE
//

IOReturn itlwm::getNOISE(IO80211Interface *interface,
                                     struct apple80211_noise_data *nd) {
    
    bzero(nd, sizeof(*nd));
    nd->version = APPLE80211_VERSION;
    nd->num_radios = 1;
    nd->noise[0] = -101;
    nd->aggregate_noise = -101;
    nd->noise_unit = APPLE80211_UNIT_DBM;
    return kIOReturnSuccess;
}

//
// MARK: 18 - INT_MIT
//
IOReturn itlwm::getINT_MIT(IO80211Interface* interface,
                                       struct apple80211_intmit_data* imd) {
    imd->version = APPLE80211_VERSION;
    imd->int_mit = APPLE80211_INT_MIT_AUTO;
    return kIOReturnSuccess;
}


//
// MARK: 19 - POWER
//

IOReturn itlwm::getPOWER(IO80211Interface *interface,
                                     struct apple80211_power_data *pd) {
    pd->version = APPLE80211_VERSION;
    pd->num_radios = 1;
//    pd->power_state[0] = dev->powerState();
    
    return kIOReturnSuccess;
}

IOReturn itlwm::setPOWER(IO80211Interface *interface,
                                     struct apple80211_power_data *pd) {
    if (pd->num_radios > 0) {
//        dev->setPowerState(pd->power_state[0]);
    }
    //fInterface->postMessage(APPLE80211_M_POWER_CHANGED, NULL, 0);
    
    return kIOReturnSuccess;
}

//
// MARK: 20 - ASSOCIATE
//

IOReturn itlwm::setASSOCIATE(IO80211Interface *interface,
                                         struct apple80211_assoc_data *ad) {
    IOLog("setAssociate %s", ad->ad_ssid);
    fInterface->setLinkState(IO80211LinkState::kIO80211NetworkLinkUp, 0);
    return kIOReturnSuccess;
}

//
// MARK: 27 - SUPPORTED_CHANNELS
//

IOReturn itlwm::getSUPPORTED_CHANNELS(IO80211Interface *interface,
                                                  struct apple80211_sup_channel_data *ad) {
    ad->version = APPLE80211_VERSION;
    ad->num_channels = 1;
    ad->supported_channels[0] = fake_channel;
    return kIOReturnSuccess;
}

//
// MARK: 28 - LOCALE
//

IOReturn itlwm::getLOCALE(IO80211Interface *interface,
                                      struct apple80211_locale_data *ld) {
    ld->version = APPLE80211_VERSION;
    ld->locale  = APPLE80211_LOCALE_FCC;
    
    return kIOReturnSuccess;
}

//
// MARK: 37 - TX_ANTENNA
//
IOReturn itlwm::getTX_ANTENNA(IO80211Interface *interface,
                                          apple80211_antenna_data *ad) {
    ad->version = APPLE80211_VERSION;
    ad->num_radios = 1;
    ad->antenna_index[0] = 1;
    return kIOReturnSuccess;
}

//
// MARK: 39 - ANTENNA_DIVERSITY
//

IOReturn itlwm::getANTENNA_DIVERSITY(IO80211Interface *interface,
                                                 apple80211_antenna_data *ad) {
    ad->version = APPLE80211_VERSION;
    ad->num_radios = 1;
    ad->antenna_index[0] = 1;
    return kIOReturnSuccess;
}

//
// MARK: 43 - DRIVER_VERSION
//

IOReturn itlwm::getDRIVER_VERSION(IO80211Interface *interface,
                                              struct apple80211_version_data *hv) {
    hv->version = APPLE80211_VERSION;
    strncpy(hv->string, fake_drv_version, sizeof(hv->string));
    hv->string_len = strlen(fake_drv_version);
    return kIOReturnSuccess;
}

//
// MARK: 44 - HARDWARE_VERSION
//

IOReturn itlwm::getHARDWARE_VERSION(IO80211Interface *interface,
                                                struct apple80211_version_data *hv) {
    hv->version = APPLE80211_VERSION;
    strncpy(hv->string, fake_hw_version, sizeof(hv->string));
    hv->string_len = strlen(fake_hw_version);
    return kIOReturnSuccess;
}

//
// MARK: 51 - COUNTRY_CODE
//

IOReturn itlwm::getCOUNTRY_CODE(IO80211Interface *interface,
                                            struct apple80211_country_code_data *cd) {
    cd->version = APPLE80211_VERSION;
    strncpy((char*)cd->cc, fake_country_code, sizeof(cd->cc));
    return kIOReturnSuccess;
}

//
// MARK: 57 - MCS
//
IOReturn itlwm::getMCS(IO80211Interface* interface, struct apple80211_mcs_data* md) {
    md->version = APPLE80211_VERSION;
    md->index = APPLE80211_MCS_INDEX_AUTO;
    return kIOReturnSuccess;
}

IOReturn itlwm::getROAM_THRESH(IO80211Interface* interface, struct apple80211_roam_threshold_data* md) {
    md->threshold = 1000;
    md->count = 0;
    return kIOReturnSuccess;
}

IOReturn itlwm::getRADIO_INFO(IO80211Interface* interface, struct apple80211_radio_info_data* md)
{
    md->version = 1;
    md->count = 1;
    return kIOReturnSuccess;
}