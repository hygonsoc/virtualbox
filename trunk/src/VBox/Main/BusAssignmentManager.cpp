/* $Id$ */

/** @file
 *
 * VirtualBox bus slots assignment manager
 */

/*
 * Copyright (C) 2010 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */
#include "BusAssignmentManager.h"

#include <iprt/asm.h>

#include <VBox/cfgm.h>

#include <map>
#include <vector>
#include <algorithm>

struct DeviceAssignmentRule
{
    const char* pszName;
    int         iBus;
    int         iDevice;
    int         iFn;
    int         iPriority;
};

struct DeviceAliasRule
{
    const char* pszDevName;
    const char* pszDevAlias;
};

/* Those rules define PCI slots assignment */

/* Device           Bus  Device Function Priority */

/* Generic rules */
static const DeviceAssignmentRule aGenericRules[] =
{
    /* VGA controller */
    {"vga",           0,  2, 0,  0},

    /* VMM device */
    {"VMMDev",        0,  4, 0,  0},

    /* Audio controllers */
    {"ichac97",       0,  5, 0,  0},
    {"hda",           0,  5, 0,  0},

    /* Storage controllers */
    {"ahci",          0, 13, 0,  1},
    {"lsilogic",      0, 20, 0,  1},
    {"buslogic",      0, 21, 0,  1},
    {"lsilogicsas",   0, 22, 0,  1},

    /* USB controllers */
    {"usb-ohci",      0,  6,  0, 0},
    {"usb-ehci",      0, 11,  0, 0},

    /* ACPI controller */
    {"acpi",          0,  7,  0, 0},

    /* Network controllers */
    /* the first network card gets the PCI ID 3, the next 3 gets 8..10,
     * next 4 get 16..19. */
    {"nic",           0,  3,  0, 1},
    {"nic",           0,  8,  0, 1},
    {"nic",           0,  9,  0, 1},
    {"nic",           0, 10,  0, 1},
    {"nic",           0, 16,  0, 1},
    {"nic",           0, 17,  0, 1},
    {"nic",           0, 18,  0, 1},
    {"nic",           0, 19,  0, 1},
    /* VMWare assigns first NIC to slot 11 */
    {"nic-vmware",    0, 11,  0, 1},

    /* ISA/LPC controller */
    {"lpc",           0, 31,  0, 0},

    { NULL,          -1, -1, -1,  0}
};

/* PIIX3 chipset rules */
static const DeviceAssignmentRule aPiix3Rules[] =
{
    {"piix3ide",      0,  1,  1, 0},
    {"pcibridge",     0, 24,  0, 0},
    {"pcibridge",     0, 25,  0, 0},
    { NULL,          -1, -1, -1, 0}
};


/* ICH9 chipset rules */
static const DeviceAssignmentRule aIch9Rules[] =
{
    /* Host Controller */
    {"i82801",        0, 30, 0,  0},

    /* Those are functions of LPC at 00:1e:00 */
    /**
     *  Please note, that for devices being functions, like we do here, device 0
     *  must be multifunction, i.e. have header type 0x80. Our LPC device is.
     *  Alternative approach is to assign separate slot to each device.
     */
    {"piix3ide",      0, 31, 1,  1},
    {"ahci",          0, 31, 2,  1},
    {"smbus",         0, 31, 3,  1},
    {"usb-ohci",      0, 31, 4,  1},
    {"usb-ehci",      0, 31, 5,  1},
    {"thermal",       0, 31, 6,  1},

    /* to make sure rule never used before rules assigning devices on it */
    {"ich9pcibridge", 0, 24, 0,  10},
    {"ich9pcibridge", 0, 25, 0,  10},
    {"ich9pcibridge", 1, 24, 0,   9},
    {"ich9pcibridge", 1, 25, 0,   9},
    {"ich9pcibridge", 2, 24, 0,   8},
    {"ich9pcibridge", 2, 25, 0,   8},
    {"ich9pcibridge", 3, 24, 0,   7},
    {"ich9pcibridge", 3, 25, 0,   7},
    {"ich9pcibridge", 4, 24, 0,   6},
    {"ich9pcibridge", 4, 25, 0,   6},
    {"ich9pcibridge", 5, 24, 0,   5},
    {"ich9pcibridge", 5, 25, 0,   5},

    /* Storage controllers */
    {"ahci",          1,  0, 0,   0},
    {"ahci",          1,  1, 0,   0},
    {"ahci",          1,  2, 0,   0},
    {"ahci",          1,  3, 0,   0},
    {"ahci",          1,  4, 0,   0},
    {"ahci",          1,  5, 0,   0},
    {"ahci",          1,  6, 0,   0},
    {"lsilogic",      1,  7, 0,   0},
    {"lsilogic",      1,  8, 0,   0},
    {"lsilogic",      1,  9, 0,   0},
    {"lsilogic",      1, 10, 0,   0},
    {"lsilogic",      1, 11, 0,   0},
    {"lsilogic",      1, 12, 0,   0},
    {"lsilogic",      1, 13, 0,   0},
    {"buslogic",      1, 14, 0,   0},
    {"buslogic",      1, 15, 0,   0},
    {"buslogic",      1, 16, 0,   0},
    {"buslogic",      1, 17, 0,   0},
    {"buslogic",      1, 18, 0,   0},
    {"buslogic",      1, 19, 0,   0},
    {"buslogic",      1, 20, 0,   0},
    {"lsilogicsas",   1, 21, 0,   0},
    {"lsilogicsas",   1, 26, 0,   0},
    {"lsilogicsas",   1, 27, 0,   0},
    {"lsilogicsas",   1, 28, 0,   0},
    {"lsilogicsas",   1, 29, 0,   0},
    {"lsilogicsas",   1, 30, 0,   0},
    {"lsilogicsas",   1, 31, 0,   0},

    /* NICs */
    {"nic",           2,  0, 0,   0},
    {"nic",           2,  1, 0,   0},
    {"nic",           2,  2, 0,   0},
    {"nic",           2,  3, 0,   0},
    {"nic",           2,  4, 0,   0},
    {"nic",           2,  5, 0,   0},
    {"nic",           2,  6, 0,   0},
    {"nic",           2,  7, 0,   0},
    {"nic",           2,  8, 0,   0},
    {"nic",           2,  9, 0,   0},
    {"nic",           2, 10, 0,   0},
    {"nic",           2, 11, 0,   0},
    {"nic",           2, 12, 0,   0},
    {"nic",           2, 13, 0,   0},
    {"nic",           2, 14, 0,   0},
    {"nic",           2, 15, 0,   0},
    {"nic",           2, 16, 0,   0},
    {"nic",           2, 17, 0,   0},
    {"nic",           2, 18, 0,   0},
    {"nic",           2, 19, 0,   0},
    {"nic",           2, 20, 0,   0},
    {"nic",           2, 21, 0,   0},
    {"nic",           2, 26, 0,   0},
    {"nic",           2, 27, 0,   0},
    {"nic",           2, 28, 0,   0},
    {"nic",           2, 29, 0,   0},
    {"nic",           2, 30, 0,   0},
    {"nic",           2, 31, 0,   0},

    { NULL,          -1, -1, -1,  0}
};

/* Aliasing rules */
static const DeviceAliasRule aDeviceAliases[] =
{
    {"e1000",       "nic"},
    {"pcnet",       "nic"},
    {"virtio-net",  "nic"},
    {"ahci",        "storage"},
    {"lsilogic",    "storage"},
    {"buslogic",    "storage"},
    {"lsilogicsas", "storage"}
};

struct BusAssignmentManager::State
{
    struct PciDeviceRecord
    {
        char szDevName[16];

        PciDeviceRecord(const char* pszName)
        {
            ::strncpy(szDevName, pszName, sizeof(szDevName));
        }

        bool operator<(const PciDeviceRecord &a) const
        {
            return ::strcmp(szDevName, a.szDevName) < 0;
        }

        bool operator==(const PciDeviceRecord &a) const
        {
            return ::strcmp(szDevName, a.szDevName) == 0;
        }
    };

    typedef std::map <PciBusAddress,PciDeviceRecord > PciMap;
    typedef std::vector<PciBusAddress>                PciAddrList;
    typedef std::vector<const DeviceAssignmentRule*>  PciRulesList;
    typedef std::map <PciDeviceRecord,PciAddrList >   ReversePciMap;

    volatile int32_t cRefCnt;
    ChipsetType_T    mChipsetType;
    PciMap           mPciMap;
    ReversePciMap    mReversePciMap;

    State()
        : cRefCnt(1), mChipsetType(ChipsetType_Null)
    {}
    ~State()
    {}

    HRESULT init(ChipsetType_T chipsetType);

    HRESULT record(const char* pszName, PciBusAddress& Address);
    HRESULT autoAssign(const char* pszName, PciBusAddress& Address);
    bool    checkAvailable(PciBusAddress& Address);
    bool    findPciAddress(const char* pszDevName, int iInstance, PciBusAddress& Address);

    const char* findAlias(const char* pszName);
    void addMatchingRules(const char* pszName, PciRulesList& aList);
};

HRESULT BusAssignmentManager::State::init(ChipsetType_T chipsetType)
{
    mChipsetType = chipsetType;
    return S_OK;
}

HRESULT BusAssignmentManager::State::record(const char* pszName, PciBusAddress& Address)
{
    PciDeviceRecord devRec(pszName);

    /* Remember address -> device mapping */
    mPciMap.insert(PciMap::value_type(Address, devRec));

    ReversePciMap::iterator it = mReversePciMap.find(devRec);
    if (it == mReversePciMap.end())
    {
        mReversePciMap.insert(ReversePciMap::value_type(devRec, PciAddrList()));
        it = mReversePciMap.find(devRec);
    }

    /* Remember device name -> addresses mapping */
    it->second.push_back(Address);

    return S_OK;
}

bool    BusAssignmentManager::State::findPciAddress(const char* pszDevName, int iInstance, PciBusAddress& Address)
{
    PciDeviceRecord devRec(pszDevName);

    ReversePciMap::iterator it = mReversePciMap.find(devRec);
    if (it == mReversePciMap.end())
        return false;

    if (iInstance >= (int)it->second.size())
        return false;

    Address = it->second[iInstance];
    return true;
}

void BusAssignmentManager::State::addMatchingRules(const char* pszName, PciRulesList& aList)
{
    size_t iRuleset, iRule;
    const DeviceAssignmentRule* aArrays[2] = {aGenericRules, NULL};

    switch (mChipsetType)
    {
        case ChipsetType_PIIX3:
            aArrays[1] = aPiix3Rules;
            break;
        case ChipsetType_ICH9:
            aArrays[1] = aIch9Rules;
            break;
        default:
            Assert(false);
            break;
    }

    for (iRuleset = 0; iRuleset < RT_ELEMENTS(aArrays); iRuleset++)
    {
        if (aArrays[iRuleset] == NULL)
            continue;

        for (iRule = 0; aArrays[iRuleset][iRule].pszName != NULL; iRule++)
        {
            if (strcmp(pszName, aArrays[iRuleset][iRule].pszName) == 0)
                aList.push_back(&aArrays[iRuleset][iRule]);
        }
    }
}

const char* BusAssignmentManager::State::findAlias(const char* pszDev)
{
    for (size_t iAlias = 0; iAlias < RT_ELEMENTS(aDeviceAliases); iAlias++)
    {
        if (strcmp(pszDev, aDeviceAliases[iAlias].pszDevName) == 0)
            return aDeviceAliases[iAlias].pszDevAlias;
    }
    return NULL;
}

static bool  RuleComparator(const DeviceAssignmentRule* r1, const DeviceAssignmentRule* r2)
{
    return (r1->iPriority > r2->iPriority);
}

HRESULT BusAssignmentManager::State::autoAssign(const char* pszName, PciBusAddress& Address)
{
    PciRulesList matchingRules;

    addMatchingRules(pszName,  matchingRules);
    const char* pszAlias = findAlias(pszName);
    if (pszAlias)
        addMatchingRules(pszAlias, matchingRules);

    AssertMsg(matchingRules.size() > 0, ("No rule for %s(%s)\n", pszName, pszAlias));

    sort(matchingRules.begin(), matchingRules.end(), RuleComparator);

    for (size_t iRule = 0; iRule < matchingRules.size(); iRule++)
    {
        const DeviceAssignmentRule* rule = matchingRules[iRule];

        Address.iBus = rule->iBus;
        Address.iDevice = rule->iDevice;
        Address.iFn = rule->iFn;

        if (checkAvailable(Address))
            return S_OK;
    }
    AssertMsg(false, ("All possible candidate positions for %s exhausted\n", pszName));

    return E_INVALIDARG;
}

bool BusAssignmentManager::State::checkAvailable(PciBusAddress& Address)
{
    PciMap::const_iterator it = mPciMap.find(Address);

    return (it == mPciMap.end());
}

BusAssignmentManager::BusAssignmentManager()
    : pState(NULL)
{
    pState = new State();
    Assert(pState);
}

BusAssignmentManager::~BusAssignmentManager()
{
    if (pState)
    {
        delete pState;
        pState = NULL;
    }
}


BusAssignmentManager* BusAssignmentManager::pInstance = NULL;

BusAssignmentManager* BusAssignmentManager::getInstance(ChipsetType_T chipsetType)
{
    if (pInstance == NULL)
    {
        pInstance = new BusAssignmentManager();
        pInstance->pState->init(chipsetType);
        Assert(pInstance);
        return pInstance;
    }

    pInstance->AddRef();
    return pInstance;
}

void BusAssignmentManager::AddRef()
{
    ASMAtomicIncS32(&pState->cRefCnt);
}
void BusAssignmentManager::Release()
{
    if (ASMAtomicDecS32(&pState->cRefCnt) == 0)
        delete this;
}

DECLINLINE(HRESULT) InsertConfigInteger(PCFGMNODE pCfg,  const char* pszName, uint64_t u64)
{
    int vrc = CFGMR3InsertInteger(pCfg, pszName, u64);
    if (RT_FAILURE(vrc))
        return E_INVALIDARG;

    return S_OK;
}

HRESULT BusAssignmentManager::assignPciDevice(const char* pszDevName, PCFGMNODE pCfg,
                                              PciBusAddress& Address,    bool fAddressRequired)
{
    HRESULT rc = S_OK;

    if (!Address.valid())
        rc = pState->autoAssign(pszDevName, Address);
    else
    {
        bool fAvailable = pState->checkAvailable(Address);

        if (!fAvailable)
        {
            if (fAddressRequired)
                rc = E_ACCESSDENIED;
            else
                rc = pState->autoAssign(pszDevName, Address);
        }
    }

    if (FAILED(rc))
        return rc;

    Assert(Address.valid() && pState->checkAvailable(Address));

    rc = pState->record(pszDevName, Address);
    if (FAILED(rc))
        return rc;

    rc = InsertConfigInteger(pCfg, "PCIBusNo",             Address.iBus);
    if (FAILED(rc))
        return rc;
    rc = InsertConfigInteger(pCfg, "PCIDeviceNo",          Address.iDevice);
    if (FAILED(rc))
        return rc;
    rc = InsertConfigInteger(pCfg, "PCIFunctionNo",        Address.iFn);
    if (FAILED(rc))
        return rc;

    return S_OK;
}


bool BusAssignmentManager::findPciAddress(const char* pszDevName, int iInstance, PciBusAddress& Address)
{
    return pState->findPciAddress(pszDevName, iInstance, Address);
}
