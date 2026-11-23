/* Tests for LogicStackingConfig::relink(): stacked analyzers that were
 * unplugged are found again by the USB port their unique id names. */
#include "pv/logicstackingconfig.h"

#include <cstring>
#include <iostream>
#include <vector>

using namespace pv;

namespace {

int failures = 0;

void check(bool ok, const char *what)
{
    if (!ok){
        std::cerr << "FAIL: " << what << std::endl;
        failures++;
    }
}

ds_device_base_info device(ds_device_handle handle, const char *unique_id)
{
    ds_device_base_info info;
    memset(&info, 0, sizeof(info));
    info.handle = handle;
    strncpy(info.name, "DSLogic U3Pro32", sizeof(info.name) - 1);
    strncpy(info.unique_id, unique_id, sizeof(info.unique_id) - 1);
    return info;
}

// Master at 8-3.2.4.1 (handle 10), secondaries at 8-3.2.3.4 (11) and 8-3.2.3.3 (12)
LogicStackingConfig three_analyzers()
{
    LogicStackingConfig config;
    config.enabled = true;
    config.master_handle = 10;
    config.master_unique_id = "USB 8-3.2.4.1";
    config.secondaries.resize(2);
    config.secondaries[0].handle = 11;
    config.secondaries[0].unique_id = "USB 8-3.2.3.4";
    config.secondaries[1].handle = 12;
    config.secondaries[1].unique_id = "USB 8-3.2.3.3";
    return config;
}

void check_port_id()
{
    check(LogicStackingConfig::is_port_id("USB 8-3.2.4.1"), "a port path is a port id");
    check(LogicStackingConfig::is_port_id("USB 20-3"), "a root port is a port id");
    check(!LogicStackingConfig::is_port_id("USB 8.61"), "bus and address are no port id");
    check(!LogicStackingConfig::is_port_id(""), "an empty id is no port id");
}

void check_present_devices_keep_their_place()
{
    LogicStackingConfig config = three_analyzers();
    config.secondaries[0].unique_id = "";
    std::vector<ds_device_base_info> devices = {
        device(1, ""), device(10, "USB 8-3.2.4.1"), device(11, "USB 8-3.2.3.4"), device(12, "USB 8-3.2.3.3")};

    check(!config.relink(devices.data(), (int)devices.size()), "listed analyzers change no handle");
    check(config.secondaries[0].unique_id == "USB 8-3.2.3.4", "a listed analyzer's port is noted");
    check(config.secondaries[0].name == "DSLogic U3Pro32", "a listed analyzer's name is noted");
}

void check_replugged_secondary()
{
    LogicStackingConfig config = three_analyzers();
    std::vector<ds_device_base_info> unplugged = {
        device(1, ""), device(10, "USB 8-3.2.4.1"), device(12, "USB 8-3.2.3.3")};

    check(!config.relink(unplugged.data(), (int)unplugged.size()), "an unplugged analyzer changes nothing");
    check(config.secondaries[0].handle == 11, "an unplugged analyzer keeps its handle");
    check(config.secondaries[0].unique_id == "USB 8-3.2.3.4", "an unplugged analyzer keeps its port");

    std::vector<ds_device_base_info> replugged = unplugged;
    replugged.push_back(device(21, "USB 8-3.2.3.4"));
    check(config.relink(replugged.data(), (int)replugged.size()), "a replugged analyzer is linked again");
    check(config.secondaries[0].handle == 21, "a replugged analyzer gets the device at its port");
    check(config.master_handle == 10 && config.secondaries[1].handle == 12, "the others keep their devices");
    check(config.is_valid(), "the relinked setup is valid");
}

void check_other_port()
{
    LogicStackingConfig config = three_analyzers();
    std::vector<ds_device_base_info> devices = {
        device(10, "USB 8-3.2.4.1"), device(12, "USB 8-3.2.3.3"), device(21, "USB 8-3.2.3.1")};

    check(!config.relink(devices.data(), (int)devices.size()), "a device at another port isn't taken");
    check(config.secondaries[0].handle == 11, "the analyzer keeps waiting for its port");
}

void check_hub_replugged()
{
    // A session loaded while the analyzers were away leaves their handles empty
    LogicStackingConfig config = three_analyzers();
    config.master_handle = NULL_HANDLE;
    config.secondaries[0].handle = NULL_HANDLE;
    config.secondaries[1].handle = NULL_HANDLE;
    std::vector<ds_device_base_info> devices = {
        device(1, ""), device(32, "USB 8-3.2.3.3"), device(30, "USB 8-3.2.4.1"), device(31, "USB 8-3.2.3.4")};

    check(config.relink(devices.data(), (int)devices.size()), "analyzers without devices are linked");
    check(config.master_handle == 30, "the master gets the device at its port");
    check(config.secondaries[0].handle == 31 && config.secondaries[1].handle == 32,
          "each secondary gets the device at its own port");
}

void check_one_device_per_analyzer()
{
    // Two analyzers claiming one port, as an edited session could: one device each at most
    LogicStackingConfig config = three_analyzers();
    config.secondaries[1].unique_id = "USB 8-3.2.3.4";
    config.secondaries[0].handle = NULL_HANDLE;
    config.secondaries[1].handle = NULL_HANDLE;
    std::vector<ds_device_base_info> devices = {device(10, "USB 8-3.2.4.1"), device(21, "USB 8-3.2.3.4")};

    check(config.relink(devices.data(), (int)devices.size()), "the device at the port is linked");
    check(config.secondaries[0].handle == 21, "the first analyzer gets it");
    check(config.secondaries[1].handle == NULL_HANDLE, "the second doesn't get it too");
}

void check_bus_address_ids()
{
    // Without a port path the id changes on every reconnect and can name another device
    LogicStackingConfig config = three_analyzers();
    config.secondaries[0].unique_id = "USB 8.61";
    std::vector<ds_device_base_info> devices = {
        device(10, "USB 8-3.2.4.1"), device(12, "USB 8-3.2.3.3"), device(21, "USB 8.61")};

    check(!config.relink(devices.data(), (int)devices.size()), "a bus and address id isn't relinked");
    check(config.secondaries[0].handle == 11, "the analyzer keeps its handle");
}

} // namespace

int main()
{
    check_port_id();
    check_present_devices_keep_their_place();
    check_replugged_secondary();
    check_other_port();
    check_hub_replugged();
    check_one_device_per_analyzer();
    check_bus_address_ids();

    if (failures != 0){
        std::cerr << failures << " check(s) failed" << std::endl;
        return 1;
    }

    std::cout << "stacking relink: all checks passed" << std::endl;
    return 0;
}
