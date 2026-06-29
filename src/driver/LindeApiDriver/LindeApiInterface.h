#pragma once

#include "driver/BusInterface.h"
#include "LindeSharedDevice.h"
#include "core/MeasurementInterface.h"

#include <atomic>
#include <cstdint>
#include <memory>

class LindeApiDriver;

class LindeApiInterface : public BusInterface
{
    Q_OBJECT

public:
    LindeApiInterface(LindeApiDriver *driver,
                      std::shared_ptr<LindeSharedDevice> sharedDev,
                      uint8_t channel);
    ~LindeApiInterface() override;

    QString getName()       const override;
    QString getDetailsStr() const override;
    BusType busType()       const override { return BusType::LIN; }

    void     applyConfig(const MeasurementInterface &mi) override;
    unsigned getBitrate() override;
    uint32_t getCapabilities() override;
    QList<CanTiming> getAvailableBitrates() override;

    void open()    override;
    void close()   override;
    bool isOpen()  override;

    void sendMessage(const BusMessage &msg) override;
    bool readMessage(QList<BusMessage> &msglist, unsigned int timeout_ms) override;

    void sendLinSleepWakeup(bool wakeup) override;
    void setLinScheduleTable(uint8_t tableIndex) override;
    void sendLinDiagRequest(uint8_t nad, const uint8_t *data, uint8_t len) override;

    bool     updateStatistics() override;
    uint32_t getState() override;
    int getNumRxFrames()  override;
    int getNumRxErrors()  override;
    int getNumTxFrames()  override;
    int getNumTxErrors()  override;
    int getNumRxOverruns() override;
    int getNumTxDropped()  override;

private:
    uint8_t                            _channel;
    std::atomic<bool>                  _isOpen{false};
    std::shared_ptr<LindeSharedDevice> _sharedDev;
    MeasurementInterface               _settings;

    uint64_t _numRx{0};
    uint64_t _numTx{0};
    uint64_t _numTxErr{0};

    // schedule entry counts per table (populated during open)
    static constexpr uint8_t MAX_TABLES = 8;
    uint8_t _tableEntryCounts[MAX_TABLES]{};
    uint8_t _activeTable{0};
};
