/*
  Copyright (c) 2026 Schildkroet

  This file is part of cangaroo.

  cangaroo is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 2 of the License, or
  (at your option) any later version.

  cangaroo is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with cangaroo.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "CandleApiInterface.h"
#include "CandleApiDriver.h"

#include <QDateTime>
#include <QMutexLocker>

#include <algorithm>

CandleApiInterface::CandleApiInterface(CandleApiDriver *driver,
                                       std::shared_ptr<CandleSharedDevice> sharedDev,
                                       uint8_t channel)
  : BusInterface(reinterpret_cast<CanDriver*>(driver)),
    _channel(channel),
    _isOpen(false),
    _isFdEnabled(false),
    _sharedDev(std::move(sharedDev)),
    _backend(driver->backend()),
    _numRx(0),
    _numTx(0),
    _numTxErr(0)
{
    _settings.setBitrate(500000);
    _settings.setSamplePoint(875);


    // Timings for 170MHz processors (CANable 2.0)
    // Tseg1: 2..256 Tseg2: 2..128 sjw: 1..128 brp: 1..512
    // Note: as expressed below, Tseg1 does not include 1 count for prop phase
    _timings
        << CandleApiTiming(170000000,   10000, 875, 68, 217, 31)
        << CandleApiTiming(170000000,   20000, 875, 34, 217, 31)
        << CandleApiTiming(170000000,   50000, 875, 17, 173, 25)
        << CandleApiTiming(170000000,   83333, 875,  8, 221, 32)
        << CandleApiTiming(170000000,  100000, 875, 10, 147, 21)
        << CandleApiTiming(170000000,  125000, 875, 8,  147, 21)
        << CandleApiTiming(170000000,  250000, 875, 4,  147, 21)
        << CandleApiTiming(170000000,  500000, 875, 2,  147, 21)
        << CandleApiTiming(170000000, 1000000, 875, 1,  147, 21);

    // Timings for 160MHz processors (CANable 2.5)
    // total_TQ = 1(sync) + 1(prop) + phase_seg1 + phase_seg2
    // bitrate  = 160MHz / (brp * total_TQ),  SP = (2 + phase_seg1) / total_TQ
    _timings
        << CandleApiTiming(160000000,   10000, 875, 80, 173, 25)
        << CandleApiTiming(160000000,   20000, 875, 40, 173, 25)
        << CandleApiTiming(160000000,   50000, 875, 16, 173, 25)
        << CandleApiTiming(160000000,   83333, 875,  8, 208, 30)
        << CandleApiTiming(160000000,  100000, 875, 10, 138, 20)
        << CandleApiTiming(160000000,  125000, 875,  8, 138, 20)
        << CandleApiTiming(160000000,  250000, 875,  4, 138, 20)
        << CandleApiTiming(160000000,  500000, 875,  2, 138, 20)
        << CandleApiTiming(160000000, 1000000, 875,  1, 138, 20);

    _timings
        // Timings for 80MHz (CANnectivity)
        // Formula: bitrate = 80MHz / (brp * (1 + prop_seg + phase_seg1 + phase_seg2))
        //          prop_seg = 1 (fixed); SP = (1 + prop_seg + phase_seg1) / total_TQ
        << CandleApiTiming(80000000,   10000, 870, 80,  85, 13)  // 80M/(80*100)  = 10k,  SP=87.0%
        << CandleApiTiming(80000000,   20000, 870, 40,  85, 13)  // 80M/(40*100)  = 20k,  SP=87.0%
        << CandleApiTiming(80000000,   50000, 870, 16,  85, 13)  // 80M/(16*100)  = 50k,  SP=87.0%
        << CandleApiTiming(80000000,  100000, 870,  8,  85, 13)  // 80M/(8*100)   = 100k, SP=87.0%
        << CandleApiTiming(80000000,  125000, 875,  5, 110, 16)  // 80M/(5*128)   = 125k, SP=87.5%
        << CandleApiTiming(80000000,  250000, 875,  2, 138, 20)  // 80M/(2*160)   = 250k, SP=87.5%
        << CandleApiTiming(80000000,  500000, 875,  1, 138, 20)  // 80M/(1*160)   = 500k, SP=87.5%
        << CandleApiTiming(80000000,  800000, 870,  1,  85, 13)  // 80M/(1*100)   = 800k, SP=87.0%
        << CandleApiTiming(80000000, 1000000, 875,  1,  68, 10)  // 80M/(1*80)    = 1M,   SP=87.5%
        << CandleApiTiming(80000000, 2000000, 875,  1,  33,  5)  // 80M/(1*40)    = 2M,   SP=87.5%
        << CandleApiTiming(80000000, 4000000, 850,  1,  15,  3)  // 80M/(1*20)    = 4M,   SP=85.0%
        << CandleApiTiming(80000000, 5000000, 875,  1,  12,  2)  // 80M/(1*16)    = 5M,   SP=87.5%
        << CandleApiTiming(80000000, 8000000, 800,  1,   6,  2); // 80M/(1*10)    = 8M,   SP=80.0%

    // Timings for 48MHz processors (CANable 0.X)
    _timings
        // sample point: 50.0%
        << CandleApiTiming(48000000,   10000, 500, 300, 6, 8)
        << CandleApiTiming(48000000,   20000, 500, 150, 6, 8)
        << CandleApiTiming(48000000,   50000, 500,  60, 6, 8)
        << CandleApiTiming(48000000,   83333, 500,  36, 6, 8)
        << CandleApiTiming(48000000,  100000, 500,  30, 6, 8)
        << CandleApiTiming(48000000,  125000, 500,  24, 6, 8)
        << CandleApiTiming(48000000,  250000, 500,  12, 6, 8)
        << CandleApiTiming(48000000,  500000, 500,   6, 6, 8)
        << CandleApiTiming(48000000,  800000, 500,   3, 8, 10)
        << CandleApiTiming(48000000, 1000000, 500,   3, 6, 8)

        // sample point: 62.5%
        << CandleApiTiming(48000000,   10000, 625, 300, 8, 6)
        << CandleApiTiming(48000000,   20000, 625, 150, 8, 6)
        << CandleApiTiming(48000000,   50000, 625,  60, 8, 6)
        << CandleApiTiming(48000000,   83333, 625,  36, 8, 6)
        << CandleApiTiming(48000000,  100000, 625,  30, 8, 6)
        << CandleApiTiming(48000000,  125000, 625,  24, 8, 6)
        << CandleApiTiming(48000000,  250000, 625,  12, 8, 6)
        << CandleApiTiming(48000000,  500000, 625,   6, 8, 6)
        << CandleApiTiming(48000000,  800000, 600,   4, 7, 6)
        << CandleApiTiming(48000000, 1000000, 625,   3, 8, 6)

        // sample point: 75.0%
        << CandleApiTiming(48000000,   10000, 750, 300, 10, 4)
        << CandleApiTiming(48000000,   20000, 750, 150, 10, 4)
        << CandleApiTiming(48000000,   50000, 750,  60, 10, 4)
        << CandleApiTiming(48000000,   83333, 750,  36, 10, 4)
        << CandleApiTiming(48000000,  100000, 750,  30, 10, 4)
        << CandleApiTiming(48000000,  125000, 750,  24, 10, 4)
        << CandleApiTiming(48000000,  250000, 750,  12, 10, 4)
        << CandleApiTiming(48000000,  500000, 750,   6, 10, 4)
        << CandleApiTiming(48000000,  800000, 750,   3, 13, 5)
        << CandleApiTiming(48000000, 1000000, 750,   3, 10, 4)

        // sample point: 87.5%
        << CandleApiTiming(48000000,   10000, 875, 300, 12, 2)
        << CandleApiTiming(48000000,   20000, 875, 150, 12, 2)
        << CandleApiTiming(48000000,   50000, 875,  60, 12, 2)
        << CandleApiTiming(48000000,   83333, 875,  36, 12, 2)
        << CandleApiTiming(48000000,  100000, 875,  30, 12, 2)
        << CandleApiTiming(48000000,  125000, 875,  24, 12, 2)
        << CandleApiTiming(48000000,  250000, 875,  12, 12, 2)
        << CandleApiTiming(48000000,  500000, 875,   6, 12, 2)
        << CandleApiTiming(48000000,  800000, 867,   4, 11, 2)
        << CandleApiTiming(48000000, 1000000, 875,   3, 12, 2);


    _timings
        // sample point: 50.0%
        << CandleApiTiming(16000000,   10000, 520, 64, 11, 12)
        << CandleApiTiming(16000000,   20000, 500, 50,  6,  8)
        << CandleApiTiming(16000000,   50000, 500, 20,  6,  8)
        << CandleApiTiming(16000000,   83333, 500, 12,  6,  8)
        << CandleApiTiming(16000000,  100000, 500, 10,  6,  8)
        << CandleApiTiming(16000000,  125000, 500,  8,  6,  8)
        << CandleApiTiming(16000000,  250000, 500,  4,  6,  8)
        << CandleApiTiming(16000000,  500000, 500,  2,  6,  8)
        << CandleApiTiming(16000000,  800000, 500,  1,  8, 10)
        << CandleApiTiming(16000000, 1000000, 500,  1,  6,  8)

        // sample point: 62.5%
        << CandleApiTiming(16000000,   10000, 625, 100, 8,  6)
        << CandleApiTiming(16000000,   20000, 625, 50,  8,  6)
        << CandleApiTiming(16000000,   50000, 625, 20,  8,  6)
        << CandleApiTiming(16000000,   83333, 625, 12,  8,  6)
        << CandleApiTiming(16000000,  100000, 625, 10,  8,  6)
        << CandleApiTiming(16000000,  125000, 625,  8,  8,  6)
        << CandleApiTiming(16000000,  250000, 625,  4,  8,  6)
        << CandleApiTiming(16000000,  500000, 625,  2,  8,  6)
        << CandleApiTiming(16000000,  800000, 650,  1, 11,  7)
        << CandleApiTiming(16000000, 1000000, 625,  1,  8,  6)

        // sample point: 75.0%
        << CandleApiTiming(16000000,   20000, 750, 50, 10,  4)
        << CandleApiTiming(16000000,   50000, 750, 20, 10,  4)
        << CandleApiTiming(16000000,   83333, 750, 12, 10,  4)
        << CandleApiTiming(16000000,  100000, 750, 10, 10,  4)
        << CandleApiTiming(16000000,  125000, 750,  8, 10,  4)
        << CandleApiTiming(16000000,  250000, 750,  4, 10,  4)
        << CandleApiTiming(16000000,  500000, 750,  2, 10,  4)
        << CandleApiTiming(16000000,  800000, 750,  1, 13,  5)
        << CandleApiTiming(16000000, 1000000, 750,  1, 10,  4)

        // sample point: 87.5%
        << CandleApiTiming(16000000,   20000, 875, 50, 12,  2)
        << CandleApiTiming(16000000,   50000, 875, 20, 12,  2)
        << CandleApiTiming(16000000,   83333, 875, 12, 12,  2)
        << CandleApiTiming(16000000,  100000, 875, 10, 12,  2)
        << CandleApiTiming(16000000,  125000, 875,  8, 12,  2)
        << CandleApiTiming(16000000,  250000, 875,  4, 12,  2)
        << CandleApiTiming(16000000,  500000, 875,  2, 12,  2)
        << CandleApiTiming(16000000,  800000, 900,  1, 16,  2)
        << CandleApiTiming(16000000, 1000000, 875,  1, 12,  2);

    // CAN FD data-phase timings
    // Formula (with CandleApiTiming's fixed prop_seg=1):
    //   bitrate = fclk / (brp * (1_sync + 1_prop + phase_seg1 + phase_seg2))
    //   SP      = (1_sync + 1_prop + phase_seg1) / total_tq  (as per-mille × 1000)

    // CAN FD data-phase hardware constraint: tseg1 (=prop_seg+phase_seg1) ≤ 31, tseg2 ≤ 16, brp ≤ 32.
    // Low bitrates require a higher brp to keep tseg1 within that limit.

    // 170 MHz (CANable 2.0 — STM32G0B1)
    _fdTimings
        << CandleApiTiming(170000000,  1000000, 794,  5, 25,  7)  //  170M/(5*34)  = 1M,  SP=79.4%
        << CandleApiTiming(170000000,  2000000, 824,  5, 12,  3)  //  170M/(5*17)  = 2M,  SP=82.4%
        << CandleApiTiming(170000000,  5000000, 794,  1, 25,  7)  //  170M/(1*34)  = 5M,  SP=79.4%
        << CandleApiTiming(170000000, 10000000, 706,  1, 10,  5); //  170M/(1*17)  = 10M, SP=70.6%

    // 160 MHz (CANable 2.5 — STM32G4)
    _fdTimings
        << CandleApiTiming(160000000,  1000000, 800,  4, 30,  8)  //  160M/(4*40)  = 1M,  SP=80.0%
        << CandleApiTiming(160000000,  2000000, 800,  2, 30,  8)  //  160M/(2*40)  = 2M,  SP=80.0%
        << CandleApiTiming(160000000,  4000000, 800,  1, 30,  8)  //  160M/(1*40)  = 4M,  SP=80.0%
        << CandleApiTiming(160000000,  5000000, 812,  1, 24,  6)  //  160M/(1*32)  = 5M,  SP=81.25%
        << CandleApiTiming(160000000,  8000000, 800,  1, 14,  4); //  160M/(1*20)  = 8M,  SP=80.0%

    // 80 MHz (CANnectivity)
    _fdTimings
        << CandleApiTiming(80000000,   1000000, 800,  2, 30,  8)  //  80M/(2*40)   = 1M,  SP=80.0%
        << CandleApiTiming(80000000,   2000000, 800,  1, 30,  8)  //  80M/(1*40)   = 2M,  SP=80.0%
        << CandleApiTiming(80000000,   4000000, 800,  1, 14,  4)  //  80M/(1*20)   = 4M,  SP=80.0%
        << CandleApiTiming(80000000,   5000000, 812,  1, 11,  3)  //  80M/(1*16)   = 5M,  SP=81.25%
        << CandleApiTiming(80000000,   8000000, 800,  1,  6,  2); //  80M/(1*10)   = 8M,  SP=80.0%

    // 48 MHz (CANable 0.x — STM32F072)
    _fdTimings
        << CandleApiTiming(48000000,   1000000, 792,  2, 17,  5)  //  48M/(2*24)   = 1M,  SP=79.2%
        << CandleApiTiming(48000000,   2000000, 792,  1, 17,  5)  //  48M/(1*24)   = 2M,  SP=79.2%
        << CandleApiTiming(48000000,   3000000, 813,  1, 11,  3)  //  48M/(1*16)   = 3M,  SP=81.3%
        << CandleApiTiming(48000000,   4000000, 833,  1,  8,  2); //  48M/(1*12)   = 4M,  SP=83.3%

    // 16 MHz
    _fdTimings
        << CandleApiTiming(16000000,   1000000, 750, 1, 10,  4)  //  16M/(1*16)   = 1M,  SP=75.0%
        << CandleApiTiming(16000000,   2000000, 750, 1,  4,  2); //  16M/(1*8)    = 2M,  SP=75.0%
}

CandleApiInterface::~CandleApiInterface()
{
}

QString CandleApiInterface::getName() const
{
    return _sharedDev->productName + QString::number(_sharedDev->deviceIndex) + "_CH" + QString::number(_channel);
}

QString CandleApiInterface::getDetailsStr() const
{
    return _sharedDev->productName + " CH" + QString::number(_channel)
           + " | " + QString::fromStdWString(getPath());
}

uint8_t CandleApiInterface::getChannel() const
{
    return _channel;
}

void CandleApiInterface::applyConfig(const MeasurementInterface &mi)
{
    _settings = mi;
}

unsigned CandleApiInterface::getBitrate()
{
    return _settings.bitrate();
}

uint32_t CandleApiInterface::getCapabilities()
{
    candle_capability_t caps;

    if (candle_channel_get_capabilities(_sharedDev->handle, _channel, &caps)) {

        uint32_t retval = 0;

        if (caps.feature & CANDLE_FEATURE_LISTEN_ONLY) {
            retval |= BusInterface::capability_listen_only;
        }

        if (caps.feature & CANDLE_FEATURE_ONE_SHOT) {
            retval |= BusInterface::capability_one_shot;
        }

        if (caps.feature & CANDLE_FEATURE_TRIPLE_SAMPLE) {
            retval |= BusInterface::capability_triple_sampling;
        }

        if (caps.feature & CANDLE_FEATURE_FD) {
            retval |= BusInterface::capability_canfd;
        }

        if (caps.feature & CANDLE_FEATURE_AUTO_RESTART) {
            retval |= BusInterface::capability_auto_restart;
        }

        return retval;

    } else {
        return 0;
    }
}

QList<CanTiming> CandleApiInterface::getAvailableBitrates()
{
    QList<CanTiming> retval;

    candle_capability_t caps;
    if (!candle_channel_get_capabilities(_sharedDev->handle, _channel, &caps)) {
        log_error(tr("CandleApi: getAvailableBitrates() failed"));
        return retval;
    }

    const bool isFdCapable = (caps.feature & CANDLE_FEATURE_FD) != 0;

    int i = 0;
    if (isFdCapable) {
        // FD-capable device: expose FD timing combinations (nominal + data rate pairs)
        for (const auto &nom : _timings) {
            if (nom.getBaseClk() != caps.fclk_can)
                continue;
            // Classic-only entry (fdBitrate = 0)
            retval << CanTiming(i++, nom.getBitrate(), 0, nom.getSamplePoint());
            // FD entries: pair each nominal rate with each available data rate
            for (const auto &fd : _fdTimings) {
                if (fd.getBaseClk() == caps.fclk_can && fd.getBitrate() >= nom.getBitrate()) {
                    retval << CanTiming(i++, nom.getBitrate(), fd.getBitrate(),
                                        nom.getSamplePoint(), fd.getSamplePoint());
                }
            }
        }
    } else {
        // Classic CAN device
        for (const auto &t : _timings) {
            if (t.getBaseClk() == caps.fclk_can) {
                retval << CanTiming(i++, t.getBitrate(), 0, t.getSamplePoint());
            }
        }
    }

    return retval;
}

bool CandleApiInterface::setBitTiming(uint32_t bitrate, uint32_t samplePoint)
{
    candle_capability_t caps;
    if (!candle_channel_get_capabilities(_sharedDev->handle, _channel, &caps)) {
        return false;
    }

    for (const auto &t : _timings) {
        if ( (t.getBaseClk() == caps.fclk_can)
          && (t.getBitrate()==bitrate)
          && (t.getSamplePoint()==samplePoint) )
        {
            candle_bittiming_t timing = t.getTiming();
            bool ok = candle_channel_set_timing(_sharedDev->handle, _channel, &timing);
            if (!ok) {
                log_error(tr("CandleApi: set bittiming failed"));
            }
            return ok;
        }
    }

    log_error(tr("CandleApi: no timing entry for bitrate=%1 samplePoint=%2 fclk=%3")
        .arg(bitrate).arg(samplePoint).arg(caps.fclk_can));
    return false;
}

bool CandleApiInterface::setDataBitTiming(uint32_t bitrate, uint32_t samplePoint)
{
    candle_capability_t caps;
    if (!candle_channel_get_capabilities(_sharedDev->handle, _channel, &caps)) {
        log_error(tr("CandleApi: get capabilities failed"));
        return false;
    }

    for (const auto &t : _fdTimings) {
        if ( (t.getBaseClk() == caps.fclk_can)
          && (t.getBitrate() == bitrate)
          && (t.getSamplePoint() == samplePoint) )
        {
            candle_bittiming_t timing = t.getTiming();
            bool ok = candle_channel_set_data_timing(_sharedDev->handle, _channel, &timing);
            if (!ok) {
                log_error(tr("CandleApi: set data bittiming failed"));
            }
            return ok;
        }
    }

    log_error(tr("CandleApi: no FD timing entry for bitrate=%1 samplePoint=%2 fclk=%3")
        .arg(bitrate).arg(samplePoint).arg(caps.fclk_can));
    return false;
}

void CandleApiInterface::open()
{
    _isFdEnabled = false;
    _getStateFailed = false;

    QMutexLocker devLock(&_sharedDev->openMutex);
    const bool firstOpen = (_sharedDev->openCount == 0);

    if (firstOpen) {
        if (!candle_dev_open(_sharedDev->handle)) {
            log_error(tr("CandleApi: device open failed"));
            _isOpen = false;
            return;
        }
    }

    if (!setBitTiming(_settings.bitrate(), _settings.samplePoint())) {
        log_error(tr("CandleApi: set bitrate failed"));
        if (firstOpen) {
            candle_dev_close(_sharedDev->handle);
        }
        _isOpen = false;
        return;
    }

    uint32_t flags = 0;
    if (_settings.isListenOnlyMode()) {
        flags |= CANDLE_MODE_LISTEN_ONLY;
    }
    if (_settings.isOneShotMode()) {
        flags |= CANDLE_MODE_ONE_SHOT;
    }
    if (_settings.isTripleSampling()) {
        flags |= CANDLE_MODE_TRIPLE_SAMPLE;
    }
    if (_settings.doAutoRestart()) {
        // Vendor flag: only send it to firmware that advertises the feature,
        // other gs_usb devices may treat unknown mode bits as an error.
        candle_capability_t caps;
        if (candle_channel_get_capabilities(_sharedDev->handle, _channel, &caps)
            && (caps.feature & CANDLE_FEATURE_AUTO_RESTART)) {
            flags |= CANDLE_MODE_AUTO_RESTART;
        }
    }

    // Enable CAN FD: auto-enable on FD-capable devices even if never explicitly configured
    {
        candle_capability_t caps;
        if (candle_channel_get_capabilities(_sharedDev->handle, _channel, &caps) && (caps.feature & CANDLE_FEATURE_FD)) {
            uint32_t fdBitrate = _settings.fdBitrate();
            uint32_t fdSamplePoint = _settings.fdSamplePoint();

            // Auto-select or repair FD timing:
            // - fdBitrate==0: never configured, pick the first entry for this clock
            // - fdBitrate>0 but SP invalid: stale config (e.g. SP saved from a
            //   different bitrate), fall back to the first SP for the requested bitrate
            bool spValid = false;
            for (const auto &fd : _fdTimings) {
                if (fd.getBaseClk() == caps.fclk_can
                    && fd.getBitrate() == fdBitrate
                    && fd.getSamplePoint() == fdSamplePoint)
                {
                    spValid = true;
                    break;
                }
            }

            if (fdBitrate == 0 || !spValid) {
                for (const auto &fd : _fdTimings) {
                    if (fd.getBaseClk() == caps.fclk_can
                        && (fdBitrate == 0 || fd.getBitrate() == fdBitrate))
                    {
                        if (!spValid && fdBitrate > 0) {
                            log_warning(tr("CandleApi: FD sample point %1 invalid for %2 bit/s, using %3")
                                .arg(fdSamplePoint).arg(fdBitrate).arg(fd.getSamplePoint()));
                        }
                        fdBitrate = fd.getBitrate();
                        fdSamplePoint = fd.getSamplePoint();
                        break;
                    }
                }
            }

            if (fdBitrate > 0) {
                if (!setDataBitTiming(fdBitrate, fdSamplePoint)) {
                    log_warning(tr("CandleApi: FD data bittiming failed, falling back to classic CAN"));
                } else {
                    flags |= CANDLE_MODE_FD;
                    _isFdEnabled = true;
                }
            }
        } else if (_settings.isCanFD()) {
            log_warning(tr("CandleApi: CAN FD requested but device does not support it"));
        }
    }

    _numRx = 0;
    _numTx = 0;
    _numTxErr = 0;
    {
        // Starting the channel discards anything still queued on the device
        QMutexLocker lock(&_txMutex);
        _pendingTx.clear();
    }

    if (!candle_channel_start(_sharedDev->handle, _channel, flags)) {
        log_error(tr("CandleApi: channel %1 start failed (error %2)")
            .arg(_channel)
            .arg(candle_dev_last_error(_sharedDev->handle)));
        if (firstOpen) {
            candle_dev_close(_sharedDev->handle);
        }
        _isOpen = false;
        return;
    }

    if (firstOpen) {
        uint32_t t_dev = 0;
        const uint64_t hostNowUs =
                _backend.getUsecsAtMeasurementStart() +
                _backend.getUsecsSinceMeasurementStart();
        if (candle_dev_get_timestamp_us(_sharedDev->handle, &t_dev)) {
            _sharedDev->resetTimestampEpoch(hostNowUs, t_dev, true);
        } else {
            _sharedDev->resetTimestampEpoch(hostNowUs, 0, false);
            log_warning(tr("CandleApi: device timestamp unavailable, using host clock"));
        }
    }

    if (firstOpen) {
        _sharedDev->startReader();
    }

    _sharedDev->openCount++;
    _isOpen = true;
}

bool CandleApiInterface::isOpen()
{
    return _isOpen;
}

void CandleApiInterface::close()
{
    candle_channel_stop(_sharedDev->handle, _channel);
    {
        // Stopping the channel discards frames the device has not sent yet
        QMutexLocker lock(&_txMutex);
        _pendingTx.clear();
    }

    QMutexLocker devLock(&_sharedDev->openMutex);
    _sharedDev->openCount--;

    if (_sharedDev->openCount == 0) {
        _sharedDev->stopReader();
        candle_dev_close(_sharedDev->handle);
    }

    _isOpen = false;
}

void CandleApiInterface::sendMessage(const BusMessage &msg)
{
    // Guard against a stuck write on the other channel (e.g. device NAKing the
    // OUT endpoint due to a CAN bus error) blocking this channel indefinitely.
    // The PIPE_TRANSFER_TIMEOUT on the OUT pipe caps any single write at 500 ms;
    // tryLock gives up after 200 ms so we fail fast rather than serialise.
    if (!_sharedDev->writeMutex.tryLock(200)) {
        _numTxErr++;
        return;
    }

    bool ok = false;

    if (_isFdEnabled && (msg.isFD() || msg.getLength() > 8)) {
        candle_fd_frame_t frame{};

        frame.can_id = msg.getId();
        if (msg.isExtended()) {
            frame.can_id |= CANDLE_ID_EXTENDED;
        }

        frame.flags = CANDLE_FRAME_FLAG_FD;
        if (msg.isBRS()) {
            frame.flags |= CANDLE_FRAME_FLAG_BRS;
        }

        const uint8_t len = msg.getLength();
        frame.can_dlc = candle_len_to_dlc(len);
        for (int i = 0; i < len; i++) {
            frame.data[i] = msg.getByte(i);
        }

        ok = candle_fd_frame_send(_sharedDev->handle, _channel, &frame);
    } else {
        candle_frame_t frame{};

        frame.can_id = msg.getId();
        if (msg.isExtended()) {
            frame.can_id |= CANDLE_ID_EXTENDED;
        }
        if (msg.isRTR()) {
            frame.can_id |= CANDLE_ID_RTR;
        }

        const uint8_t len = std::min(msg.getLength(), static_cast<uint8_t>(8u));
        frame.can_dlc = len;
        for (int i = 0; i < len; i++) {
            frame.data[i] = msg.getByte(i);
        }

        ok = candle_frame_send(_sharedDev->handle, _channel, &frame);
    }

    _sharedDev->writeMutex.unlock();

    if (ok) {
        // Not shown yet: the device echoes the frame once it has actually been
        // transmitted (see readMessage()), so a frame that never makes it onto
        // the bus (e.g. no ACK with the cable unplugged) never appears as TX.
        BusMessage txMsg = msg;
        txMsg.setFD(_isFdEnabled && (msg.isFD() || msg.getLength() > 8));
        txMsg.setRX(false);
        QMutexLocker lock(&_txMutex);
        if (_pendingTx.size() >= MaxPendingTx) {
            // The device stopped echoing (e.g. channel reset): drop the oldest.
            _pendingTx.removeFirst();
            _numTxErr++;
        }
        _pendingTx.append(txMsg);
    } else {
        _numTxErr++;
    }
}

bool CandleApiInterface::takeConfirmedTx(const candle_fd_frame_t &echo, BusMessage &txMsg)
{
    const bool echoFd = (echo.flags & CANDLE_FRAME_FLAG_FD) != 0;
    const uint8_t echoDlc = echoFd ? echo.can_dlc : std::min(echo.can_dlc, static_cast<uint8_t>(8u));
    const uint32_t echoId = echo.can_id & 0x1FFFFFFFu;
    const bool echoExt = (echo.can_id & CANDLE_ID_EXTENDED) != 0;
    const bool echoRtr = (echo.can_id & CANDLE_ID_RTR) != 0;

    QMutexLocker lock(&_txMutex);

    // Echoes arrive in transmit order, but the device may have dropped a frame
    // without echoing it (channel restart, overflow): match by content and
    // count older, never confirmed entries as TX errors.
    for (int i = 0; i < _pendingTx.size(); i++) {
        const BusMessage &p = _pendingTx.at(i);
        const uint8_t sentLen = p.isFD() ? p.getLength() : std::min(p.getLength(), static_cast<uint8_t>(8u));
        const uint8_t sentDlc = p.isFD() ? candle_len_to_dlc(sentLen) : sentLen;

        if (p.getId() != echoId || p.isExtended() != echoExt || sentDlc != echoDlc) {
            continue;
        }

        bool sameData = true;
        for (int b = 0; !echoRtr && b < sentLen; b++) {
            if (p.getByte(b) != echo.data[b]) {
                sameData = false;
                break;
            }
        }
        if (!sameData) {
            continue;
        }

        txMsg = p;
        _numTxErr += static_cast<uint64_t>(i);
        _pendingTx.erase(_pendingTx.begin(), _pendingTx.begin() + i + 1);
        return true;
    }
    return false;
}

bool CandleApiInterface::readMessage(QList<BusMessage> &msglist, unsigned int timeout_ms)
{
    // The reader thread fills per-channel queues; we just wait for our channel.
    CandleQueuedFrame queuedFrame;
    if (!_sharedDev->readFrame(_channel, queuedFrame, timeout_ms)) {
        return false;
    }
    candle_fd_frame_t frame = queuedFrame.frame;
    const candle_frametype_t frameType = candle_fd_frame_type(&frame);

    const int64_t frameTs_us = queuedFrame.timestampValid
            ? static_cast<int64_t>(queuedFrame.timestampUs)
            : static_cast<int64_t>(QDateTime::currentMSecsSinceEpoch()) * 1000LL;

    if (frameType == CANDLE_FRAMETYPE_ECHO) {
        // TX confirmation: the device transmitted one of our frames. Show it
        // now, stamped with the device's transmit time.
        BusMessage txMsg;
        if (!takeConfirmedTx(frame, txMsg)) {
            return false;
        }
        _numTx++;
        txMsg.setInterfaceId(getId());
        txMsg.setTimestamp_us(frameTs_us);
        msglist.append(txMsg);
        return true;
    }

    _numRx++;

    BusMessage msg;
    msg.setInterfaceId(getId());
    msg.setId(candle_fd_frame_id(&frame));
    msg.setExtended(candle_fd_frame_is_extended_id(&frame));

    if (frameType == CANDLE_FRAMETYPE_ERROR) {
        const uint32_t errId = candle_fd_frame_id(&frame);
        const uint8_t *d = candle_fd_frame_data(&frame);
        // The ID field carries error classes, not a CAN ID: don't show or
        // decode it as one.
        msg.setId(0);
        msg.setExtended(false);
        if (errId & 0x00000001) msg.setErrorFlag(BusError::TxTimeout);
        if (errId & 0x00000020) msg.setErrorFlag(BusError::Ack);
        if (errId & 0x00000040) msg.setErrorFlag(BusError::BusOff);
        if (errId & 0x00000100) msg.setErrorFlag(BusError::Restarted);
        if (errId & 0x00000004) {
            if (d[1] & 0x03) msg.setErrorFlag(BusError::Overrun);
            if (d[1] & 0x0C) msg.setErrorFlag(BusError::ErrorWarning);
            if (d[1] & 0x30) msg.setErrorFlag(BusError::ErrorPassive);
            if (d[1] & 0x40) msg.setErrorFlag(BusError::ErrorActive);
        }
        if (errId & 0x00000008) {
            const uint8_t prot = d[2];
            if (prot & 0x01) msg.setErrorFlag(BusError::Bit);
            if (prot & 0x02) msg.setErrorFlag(BusError::Form);
            if (prot & 0x04) msg.setErrorFlag(BusError::Stuff);
            if (prot & 0x18) msg.setErrorFlag(BusError::Bit);
            if (d[3] == 0x08) msg.setErrorFlag(BusError::Crc);
        }
        // CAN_ERR_BUSERROR (0x80) or anything unclassified
        if (!msg.isErrorFrame()) msg.setErrorFlag(BusError::Generic);
    }
    msg.setRTR(candle_fd_frame_is_rtr(&frame));

    const bool isFd = candle_fd_frame_is_fd(&frame);
    msg.setFD(isFd);
    msg.setBRS(isFd && candle_fd_frame_is_brs(&frame));

    const uint8_t raw_dlc = candle_fd_frame_dlc(&frame);
    const uint8_t len = isFd ? candle_dlc_to_len(raw_dlc) : raw_dlc;
    uint8_t *data = candle_fd_frame_data(&frame);
    msg.setLength(len);
    for (int i = 0; i < len; i++) {
        msg.setByte(i, data[i]);
    }

    msg.setTimestamp_us(frameTs_us);
    msglist.append(msg);
    return true;
}

bool CandleApiInterface::updateStatistics()
{
    return true;
}

uint32_t CandleApiInterface::getState()
{
    if (_getStateFailed)
        return BusInterface::state_ok;

    candle_capability_t caps;
    if (!candle_channel_get_capabilities(_sharedDev->handle, _channel, &caps))
        return BusInterface::state_ok;
    if (!(caps.feature & CANDLE_FEATURE_GET_STATE))
        return BusInterface::state_ok;

    candle_can_state_t s;
    if (!candle_channel_get_state(_sharedDev->handle, _channel, &s)) {
        _getStateFailed = true;
        return BusInterface::state_ok;
    }

    switch (s) {
        case CANDLE_STATE_ERROR_WARNING: return BusInterface::state_warning;
        case CANDLE_STATE_ERROR_PASSIVE: return BusInterface::state_passive;
        case CANDLE_STATE_BUS_OFF:       return BusInterface::state_bus_off;
        default:                         return BusInterface::state_ok;
    }
}

int CandleApiInterface::getNumRxFrames()
{
    return _numRx;
}

int CandleApiInterface::getNumRxErrors()
{
    return 0;
}

int CandleApiInterface::getNumTxFrames()
{
    return _numTx;
}

int CandleApiInterface::getNumTxErrors()
{
    return _numTxErr;
}

int CandleApiInterface::getNumRxOverruns()
{
    return 0;
}

int CandleApiInterface::getNumTxDropped()
{
    return 0;
}

std::wstring CandleApiInterface::getPath() const
{
    return std::wstring(candle_dev_get_path(_sharedDev->handle));
}

QString CandleApiInterface::getVersion()
{
    return "1.0";
}
