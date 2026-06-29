#pragma once

#include "core/Backend.h"
#include "driver/CanDriver.h"
#include "LindeSharedDevice.h"

#include <map>
#include <memory>
#include <string>

class GenericLinSetupPage;

class LindeApiDriver : public CanDriver
{
public:
    explicit LindeApiDriver(Backend &backend);

    QString getName() const override;
    bool update() override;

private:
    GenericLinSetupPage *_setupPage;

    // Keyed by "bus:port" string to deduplicate on re-scan.
    std::map<std::string, std::shared_ptr<LindeSharedDevice>> _devices;
};
