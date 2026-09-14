#include "LindeApiDriver.h"
#include "LindeApiInterface.h"

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

    // Only devices exposing the LIN interface are counted: the VID/PID is shared
    // with gs_usb adapters, which must not hide a LIN device plugged in after them.
    const int deviceCount = LindeSharedDevice::enumerateDevices();
    for (int index = 0; index < deviceCount; index++)
    {
        auto sharedDev = std::make_shared<LindeSharedDevice>();
        sharedDev->deviceIndex = index;

        // Brief open to read the channel count; interfaces reopen on measurement start.
        if (!sharedDev->open())
        {
            log_warning(QStringLiteral("LindeAPI: cannot open device %1: %2")
                            .arg(index)
                            .arg(QString::fromStdString(sharedDev->getLastError())));
            continue;
        }
        const uint8_t channelCount = sharedDev->channelCount; // already clamped to MAX_CHANNELS
        sharedDev->close();

        _devices["linusb:" + std::to_string(index)] = sharedDev;

        for (uint8_t ch = 0; ch < channelCount; ch++)
            addInterface(new LindeApiInterface(this, sharedDev, ch));
    }

    return true;
}
