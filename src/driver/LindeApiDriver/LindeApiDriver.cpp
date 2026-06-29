#include "LindeApiDriver.h"
#include "LindeApiInterface.h"
#include "LindeApi.hpp"

#include "core/Log.h"
#include "driver/GenericLinSetupPage.h"

LindeApiDriver::LindeApiDriver(Backend &backend)
    : CanDriver(backend),
      _setupPage(new GenericLinSetupPage())
{
    QObject::connect(&backend, &Backend::onSetupDialogCreated,
                     _setupPage, &GenericLinSetupPage::onSetupDialogCreated);
}

QString LindeApiDriver::getName() const
{
    return QStringLiteral("LindeAPI");
}

bool LindeApiDriver::update()
{
    deleteAllInterfaces();
    _devices.clear();

    // Use LindeApi channel 0 to probe the device; it manages its own libusb
    // context so there are no cross-context pointer issues.
    LindeApi probe(0);
    if (!probe.open())
    {
        const std::string &err = probe.lastError();
        if (err.find("Access denied") != std::string::npos ||
            err.find("insufficient permissions") != std::string::npos)
        {
            log_warning(QStringLiteral("LindeAPI: cannot open USB device (VID 0x1d50 / PID 0x606f): "
                                       "permission denied. Add a udev rule or run as root.\n"));
        }
        return true;
    }

    lin_usb_device_config_t dcfg{};
    uint8_t channelCount = 1;
    if (probe.getDeviceConfig(dcfg))
        channelCount = static_cast<uint8_t>(dcfg.icount + 1u);

    probe.close();

    auto sharedDev = std::make_shared<LindeSharedDevice>();
    sharedDev->deviceIndex = 0;
    _devices["linusb:0"] = sharedDev;

    for (uint8_t ch = 0; ch < channelCount; ch++)
        addInterface(new LindeApiInterface(this, sharedDev, ch));

    return true;
}
