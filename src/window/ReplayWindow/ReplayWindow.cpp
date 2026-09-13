/*
  Copyright (c) 2026 Schildkroet

  This file is part of CANgaroo.

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
#include "ReplayWindow.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QSlider>
#include <QLabel>
#include <QTimer>
#include <QFileDialog>
#include <QTextStream>
#include <QDomDocument>
#include <QFileInfo>
#include <QFont>
#include <QDoubleSpinBox>
#include <QRegularExpression>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QHeaderView>

#include <QCheckBox>
#include <QComboBox>
#include <QDataStream>
#include <QProgressBar>

#include "core/BusTrace.h"
#include "core/Backend.h"
#include "core/DBC/CanDbMessage.h"
#include "core/TraceLineFormat.h"
#include "driver/BusInterface.h"


ReplayWindow::ReplayWindow(QWidget *parent, Backend &backend)
    : ConfigurableWidget(parent), _backend(&backend), _playbackIndex(0), _playing(false)
{
    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(2, 2, 2, 2);

    // Toolbar
    auto *toolbar = new QHBoxLayout();
    _btnLoad = new QPushButton(tr("Load"));
    _btnPlay = new QPushButton(tr("Play"));
    _btnStop = new QPushButton(tr("Stop"));
    _btnPlay->setEnabled(false);
    _btnStop->setEnabled(false);

    _speedSpin = new QDoubleSpinBox(this);
    _speedSpin->setRange(0.1, 10.0);
    _speedSpin->setValue(1.0);
    _speedSpin->setSingleStep(0.1);
    _speedSpin->setSuffix("x");
    _speedSpin->setToolTip(tr("Playback speed multiplier"));

    _cbAutoplay = new QCheckBox(tr("Autoplay"), this);
    _cbAutoplay->setToolTip(tr("Automatically start/stop replay with measurement"));

    _cbLoop = new QCheckBox(tr("Loop"), this);
    _cbLoop->setToolTip(tr("Restart replay after all messages were sent"));

    toolbar->addWidget(_btnLoad);
    toolbar->addWidget(_btnPlay);
    toolbar->addWidget(_btnStop);
    toolbar->addWidget(_cbAutoplay);
    toolbar->addWidget(_cbLoop);
    toolbar->addStretch();
    toolbar->addWidget(new QLabel(tr("Speed:"), this));
    toolbar->addWidget(_speedSpin);

    mainLayout->addLayout(toolbar);

    // File path display
    _fileLabel = new QLineEdit(this);
    _fileLabel->setReadOnly(true);
    _fileLabel->setPlaceholderText(tr("No trace file loaded"));
    _fileLabel->setFrame(false);
    mainLayout->addWidget(_fileLabel);

    // Slider + position label
    auto *sliderLayout = new QHBoxLayout();
    _slider = new QSlider(Qt::Horizontal, this);
    _slider->setEnabled(false);
    _posLabel = new QLabel("0 / 0", this);
    _posLabel->setMinimumWidth(100);
    sliderLayout->addWidget(_slider);
    sliderLayout->addWidget(_posLabel);
    mainLayout->addLayout(sliderLayout);

    _progressBar = new QProgressBar(this);
    _progressBar->setRange(0, 100);
    _progressBar->setValue(0);
    _progressBar->setFormat("%p%");
    _progressBar->setTextVisible(true);
    mainLayout->addWidget(_progressBar);

    // Info box
    QFont mono("Monospace");
    mono.setStyleHint(QFont::TypeWriter);

    _infoBox = new QPlainTextEdit(this);
    _infoBox->setFont(mono);
    _infoBox->setReadOnly(true);
    _infoBox->setPlaceholderText(tr("Trace file info..."));
    _infoBox->setMaximumHeight(80);
    mainLayout->addWidget(_infoBox);

    // Filter tree with select/deselect buttons
    auto *filterBtnLayout = new QHBoxLayout();
    _btnSelectAll = new QPushButton(tr("Select All"));
    _btnDeselectAll = new QPushButton(tr("Deselect All"));
    filterBtnLayout->addWidget(_btnSelectAll);
    filterBtnLayout->addWidget(_btnDeselectAll);
    filterBtnLayout->addStretch();
    mainLayout->addLayout(filterBtnLayout);

    _filterTree = new QTreeWidget(this);
    _filterTree->setHeaderLabels({tr("Interface / CAN ID"), tr("Name"), tr("RX"), tr("TX"), tr("Count"), tr("Output")});
    _filterTree->header()->setStretchLastSection(false);
    _filterTree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    _filterTree->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    _filterTree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    _filterTree->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    _filterTree->header()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    _filterTree->header()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    mainLayout->addWidget(_filterTree, 1);

    // Timer for playback
    _timer = new QTimer(this);
    _timer->setTimerType(Qt::PreciseTimer);

    // Connections
    connect(_btnLoad, &QPushButton::clicked, this, &ReplayWindow::onLoadClicked);
    connect(_btnPlay, &QPushButton::clicked, this, &ReplayWindow::onPlayClicked);
    connect(_btnStop, &QPushButton::clicked, this, &ReplayWindow::onStopClicked);
    connect(_slider, &QSlider::sliderMoved, this, &ReplayWindow::onSliderMoved);
    connect(_timer, &QTimer::timeout, this, &ReplayWindow::onTimerTick);
    connect(_btnSelectAll, &QPushButton::clicked, this, &ReplayWindow::onSelectAllClicked);
    connect(_btnDeselectAll, &QPushButton::clicked, this, &ReplayWindow::onDeselectAllClicked);
    connect(_filterTree, &QTreeWidget::itemChanged, this, &ReplayWindow::onFilterItemChanged);
    connect(_backend, &Backend::beginMeasurement, this, &ReplayWindow::onBeginMeasurement);
    connect(_backend, &Backend::endMeasurement, this, &ReplayWindow::onEndMeasurement);
}

void ReplayWindow::retranslateUi()
{
    _btnLoad->setText(tr("Load"));
    _btnPlay->setText(tr("Play"));
    _btnStop->setText(tr("Stop"));
    _btnSelectAll->setText(tr("Select All"));
    _btnDeselectAll->setText(tr("Deselect All"));
    _speedSpin->setToolTip(tr("Playback speed multiplier"));
}

void ReplayWindow::onLoadClicked()
{
    QString filename = QFileDialog::getOpenFileName(this, tr("Load Trace File"),
                                                    _traceFilePath,
                                                    tr("All Supported (*.asc *.candump *.pcap *.pcapng);;Vector ASC (*.asc);;Linux candump (*.candump);;PCAP (*.pcap);;PCAPng (*.pcapng);;All Files (*)"));
    if (filename.isEmpty())
    {
        return;
    }
    loadTraceFile(filename);
}

void ReplayWindow::loadTraceFile(const QString &filename)
{
    bool isBinary = filename.endsWith(".pcap", Qt::CaseInsensitive) || filename.endsWith(".pcapng", Qt::CaseInsensitive);

    QFile file(filename);
    if (!file.open(isBinary ? QIODevice::ReadOnly : (QIODevice::ReadOnly | QIODevice::Text)))
    {
        _infoBox->setPlainText(tr("Error: Cannot open file."));
        return;
    }

    _messages.clear();
    _messageInterfaces.clear();
    _playbackIndex = 0;
    _playing = false;
    _timer->stop();

    bool ok = false;
    if (filename.endsWith(".candump", Qt::CaseInsensitive))
    {
        ok = parseCanDump(file);
    }
    else if (filename.endsWith(".pcapng", Qt::CaseInsensitive))
    {
        ok = parsePcapNg(file);
    }
    else if (filename.endsWith(".pcap", Qt::CaseInsensitive))
    {
        ok = parsePcap(file);
    }
    else
    {
        ok = parseVectorAsc(file);
    }
    file.close();

    if (!ok || _messages.isEmpty())
    {
        _infoBox->setPlainText(tr("Error: Failed to parse trace file or file is empty."));
        _btnPlay->setEnabled(false);
        _slider->setEnabled(false);
        _filterTree->clear();
        _enabledDirs.clear();
        return;
    }

    _traceFilePath = filename;
    _fileLabel->setText(filename);

    _slider->setRange(0, _messages.size() - 1);
    _slider->setValue(0);
    _slider->setEnabled(true);
    _btnPlay->setEnabled(true);
    _btnStop->setEnabled(false);

    double duration = _messages.last().getFloatTimestamp() - _messages.first().getFloatTimestamp();
    QString info = tr("File: %1\nMessages: %2\nDuration: %3 s")
                       .arg(QFileInfo(filename).fileName())
                       .arg(_messages.size())
                       .arg(duration, 0, 'f', 3);
    _infoBox->setPlainText(info);

    buildFilterTree();
    updatePositionLabel();
}

bool ReplayWindow::parseCanDump(QFile &file)
{
    // Formats:
    //   Classic:  (timestamp) interface ID#DATA
    //   CANFD:    (timestamp) interface ID##FlagsDATA
    //   RTR:      (timestamp) interface ID#RD  (R followed by optional DLC digit)
    //   Error:    (timestamp) interface ID#DATA  (ID has error flag 0x20000000)
    QTextStream stream(&file);
    QRegularExpression re(R"(\((\d+\.\d+)\)\s+(\S+)\s+([0-9A-Fa-f]+)(##?)(.*)$)");

    while (!stream.atEnd())
    {
        QString line = stream.readLine().trimmed();
        if (line.isEmpty())
        {
            continue;
        }

        QRegularExpressionMatch match = re.match(line);
        if (!match.hasMatch())
        {
            continue;
        }

        double timestamp = match.captured(1).toDouble();
        QString interface = match.captured(2);
        uint32_t rawId = match.captured(3).toUInt(nullptr, 16);
        QString separator = match.captured(4);
        QString payload = match.captured(5);

        BusMessage msg;
        msg.setTimestamp(timestamp);

        // Error frame: error flag bit set in ID
        if (rawId & 0x20000000)
        {
            msg.setErrorFrame(true);
            msg.setId(rawId & ~0x20000000);
            msg.setLength(0);
            _messages.append(msg);
            _messageInterfaces.append(interface);
            continue;
        }

        bool extended = (rawId > 0x7FF);
        msg.setExtended(extended);
        msg.setId(rawId);
        msg.setRX(true);

        if (separator == "##")
        {
            // CANFD: first char is flags byte, rest is data
            msg.setFD(true);
            if (!payload.isEmpty())
            {
                uint8_t flags = QString(payload[0]).toUInt(nullptr, 16);
                msg.setBRS((flags & 0x1) != 0);
                QString dataStr = payload.mid(1);
                int dlc = dataStr.length() / 2;
                msg.setLength(dlc);
                for (int i = 0; i < dlc; i++)
                {
                    msg.setByte(i, dataStr.mid(i * 2, 2).toUInt(nullptr, 16));
                }
            }
        }
        else if (payload.startsWith('R') || payload.startsWith('r'))
        {
            // RTR: #R or #R8 (R followed by optional DLC)
            msg.setRTR(true);
            QString dlcStr = payload.mid(1);
            int dlc = dlcStr.isEmpty() ? 0 : dlcStr.toInt();
            msg.setLength(dlc);
        }
        else
        {
            // Classic CAN
            int dlc = payload.length() / 2;
            msg.setLength(dlc);
            for (int i = 0; i < dlc; i++)
            {
                msg.setByte(i, payload.mid(i * 2, 2).toUInt(nullptr, 16));
            }
        }

        _messages.append(msg);
        _messageInterfaces.append(interface);
    }

    return !_messages.isEmpty();
}

bool ReplayWindow::parseVectorAsc(QFile &file)
{
    // Format: timestamp channel ID dir d DLC byte0 byte1 ...
    QTextStream stream(&file);

    while (!stream.atEnd())
    {
        QString line = stream.readLine().trimmed();
        if (line.isEmpty())
        {
            continue;
        }

        // Data lines start with a number (timestamp)
        if (!line[0].isDigit())
        {
            continue;
        }

        QStringList parts = line.split(QRegularExpression("\\s+"));
        if (parts.size() < 3)
        {
            continue;
        }

        bool ok = false;
        double timestamp = parts[0].toDouble(&ok);
        if (!ok)
        {
            continue;
        }

        QString channel = parts[1];

        // Error frame: "timestamp channel ErrorFrame"
        if (parts[2].compare("ErrorFrame", Qt::CaseInsensitive) == 0)
        {
            BusMessage msg;
            msg.setTimestamp(timestamp);
            msg.setErrorFrame(true);
            msg.setLength(0);
            msg.setId(0);
            msg.setInterfaceId(channel.toInt());
            _messages.append(msg);
            _messageInterfaces.append(tr("CH %1").arg(channel));
            continue;
        }

        // LIN frame: timestamp channel LIN id dir d DLC byte0 ... checksum = XX ...
        //            timestamp channel LIN id dir LIN_ChecksumError
        if (parts.size() >= 6 && parts[2].compare("LIN", Qt::CaseInsensitive) == 0)
        {
            uint8_t linId = static_cast<uint8_t>(parts[3].toUInt(nullptr, 16) & 0x3Fu);
            QString linDir = parts[4];

            BusMessage msg;
            msg.setTimestamp(timestamp);
            msg.setBusType(BusType::LIN);
            msg.setId(linId);
            msg.setRX(linDir.compare("Rx", Qt::CaseInsensitive) == 0);

            if (parts[5].startsWith("LIN_", Qt::CaseInsensitive))
            {
                msg.setErrorFrame(true);
                msg.setLength(0);
            }
            else if (parts[5].compare("d", Qt::CaseInsensitive) == 0 && parts.size() >= 8)
            {
                int dlc = parts[6].toInt(&ok);
                if (ok)
                {
                    msg.setLength(static_cast<uint8_t>(dlc));
                    for (int i = 0; i < dlc && (7 + i) < parts.size(); i++)
                    {
                        if (parts[7 + i].compare("checksum", Qt::CaseInsensitive) == 0)
                            break;
                        msg.setByte(i, static_cast<uint8_t>(parts[7 + i].toUInt(nullptr, 16)));
                    }
                }
            }

            _messages.append(msg);
            _messageInterfaces.append(tr("CH %1").arg(channel));
            continue;
        }

        if (parts.size() < 6)
        {
            continue;
        }

        // CAN FD: timestamp CANFD channel Rx/Tx ID ... -- Vector layout as well as
        // the one older cangaroo versions wrote.
        if (parts[1].compare("CANFD", Qt::CaseInsensitive) == 0)
        {
            BusMessage msg;
            if (!TraceLineFormat::parseAscCanFdLine(parts, msg))
            {
                continue;
            }
            msg.setTimestamp(timestamp);

            _messages.append(msg);
            _messageInterfaces.append(tr("CH %1").arg(parts[2]));
            continue;
        }

        QString idStr = parts[2];
        bool extended = idStr.endsWith('x') || idStr.endsWith('X');
        if (extended)
        {
            idStr.chop(1);
        }

        uint32_t canId = idStr.toUInt(&ok, 16);
        if (!ok)
        {
            continue;
        }

        QString dir = parts[3];
        // parts[4] = "d" or "r" (data/remote frame)
        bool isRTR = (parts[4].toLower() == "r");

        int dlcIdx = 5;
        int dlc = parts[dlcIdx].toInt(&ok);
        if (!ok)
        {
            continue;
        }

        BusMessage msg;
        msg.setTimestamp(timestamp);
        msg.setExtended(extended);
        msg.setId(canId);
        msg.setRX(dir.toLower() == "rx");
        msg.setRTR(isRTR);
        msg.setLength(dlc);
        msg.setInterfaceId(channel.toInt());

        for (int i = 0; i < dlc && (dlcIdx + 1 + i) < parts.size(); i++)
        {
            msg.setByte(i, parts[dlcIdx + 1 + i].toUInt(nullptr, 16));
        }

        _messages.append(msg);
        _messageInterfaces.append(tr("CH %1").arg(channel));
    }

    return !_messages.isEmpty();
}

// Helper: parse a SocketCAN frame from a PCAP/PCAPng packet payload
static bool parseSocketCanFrame(QDataStream &ds, quint32 capturedLen,
                                BusMessage &msg, bool &valid)
{
    static const quint32 CAN_EFF_FLAG = 0x80000000;
    static const quint32 CAN_RTR_FLAG = 0x40000000;
    static const quint32 CAN_ERR_FLAG = 0x20000000;
    static const quint32 CAN_ID_MASK = 0x1FFFFFFF;

    valid = false;
    if (capturedLen < 16)
        return false;

    quint32 can_id;
    quint8 dlc_or_len, pad1, pad2, pad3;
    ds >> can_id >> dlc_or_len >> pad1 >> pad2 >> pad3;

    bool isFD = (capturedLen == 72);
    int dataLen = isFD ? 64 : 8;

    // Read data bytes
    uint8_t data[64] = {};
    int toRead = qMin(static_cast<int>(capturedLen) - 8, dataLen);
    for (int i = 0; i < toRead; i++)
    {
        quint8 b;
        ds >> b;
        data[i] = b;
    }
    // Skip remaining bytes if any
    int remaining = static_cast<int>(capturedLen) - 8 - toRead;
    for (int i = 0; i < remaining; i++)
    {
        quint8 b;
        ds >> b;
    }

    msg.setId(can_id & CAN_ID_MASK);
    msg.setExtended((can_id & CAN_EFF_FLAG) != 0);
    msg.setRTR((can_id & CAN_RTR_FLAG) != 0);
    msg.setErrorFrame((can_id & CAN_ERR_FLAG) != 0);
    msg.setRX(true);

    if (isFD)
    {
        msg.setFD(true);
        msg.setBRS((pad1 & 0x01) != 0); // pad1 is flags in canfd_frame
        msg.setLength(dlc_or_len);
    }
    else
    {
        msg.setFD(false);
        quint8 d = dlc_or_len > 8 ? 8 : dlc_or_len;
        msg.setLength(d);
    }

    for (int i = 0; i < msg.getLength(); i++)
    {
        msg.setByte(i, data[i]);
    }

    valid = true;
    return true;
}

bool ReplayWindow::parsePcap(QFile &file)
{
    // PCAP global header: magic(4) + ver_maj(2) + ver_min(2) + thiszone(4) + sigfigs(4) + snaplen(4) + linktype(4)
    static const quint32 PCAP_MAGIC_LE = 0xA1B2C3D4;
    static const quint32 PCAP_MAGIC_BE = 0xD4C3B2A1;
    static const quint32 PCAP_MAGIC_NS_LE = 0xA1B23C4D; // nanosecond variant
    static const quint32 PCAP_MAGIC_NS_BE = 0x4D3CB2A1;
    static const quint32 LINKTYPE_CAN_SOCKETCAN = 227;

    QDataStream ds(&file);

    // Read magic to determine byte order
    quint32 magic;
    ds >> magic;

    bool nanoSecond = false;
    if (magic == PCAP_MAGIC_LE || magic == PCAP_MAGIC_NS_LE)
    {
        ds.setByteOrder(QDataStream::LittleEndian);
        nanoSecond = (magic == PCAP_MAGIC_NS_LE);
    }
    else if (magic == PCAP_MAGIC_BE || magic == PCAP_MAGIC_NS_BE)
    {
        ds.setByteOrder(QDataStream::BigEndian);
        nanoSecond = (magic == PCAP_MAGIC_NS_BE);
    }
    else
    {
        return false;
    }

    quint16 verMaj, verMin;
    qint32 thiszone;
    quint32 sigfigs, snaplen, linktype;
    ds >> verMaj >> verMin >> thiszone >> sigfigs >> snaplen >> linktype;

    if (linktype != LINKTYPE_CAN_SOCKETCAN)
    {
        return false;
    }

    while (!ds.atEnd())
    {
        quint32 ts_sec, ts_frac, capturedLen, originalLen;
        ds >> ts_sec >> ts_frac >> capturedLen >> originalLen;
        if (ds.status() != QDataStream::Ok)
            break;

        int64_t ts_us;
        if (nanoSecond)
        {
            ts_us = static_cast<int64_t>(ts_sec) * 1000000 + static_cast<int64_t>(ts_frac) / 1000;
        }
        else
        {
            ts_us = static_cast<int64_t>(ts_sec) * 1000000 + static_cast<int64_t>(ts_frac);
        }

        BusMessage msg;
        bool valid;
        parseSocketCanFrame(ds, capturedLen, msg, valid);

        if (valid)
        {
            msg.setTimestamp_us(ts_us);
            _messages.append(msg);
            _messageInterfaces.append(QStringLiteral("pcap0"));
        }
    }

    return !_messages.isEmpty();
}

bool ReplayWindow::parsePcapNg(QFile &file)
{
    // PCAPng block types
    static const quint32 BT_SHB = 0x0A0D0D0A;
    static const quint32 BT_IDB = 0x00000001;
    static const quint32 BT_EPB = 0x00000006;
    static const quint32 BT_SPB = 0x00000003;
    Q_UNUSED(BT_SPB);

    static const quint32 BYTE_ORDER_MAGIC = 0x1A2B3C4D;
    static const quint32 LINKTYPE_CAN_SOCKETCAN = 227;

    QDataStream ds(&file);

    // Interface metadata collected from IDB blocks
    struct IfaceInfo
    {
        QString name;
        quint32 linkType = 0;
        quint8 tsResol = 6; // default: 10^-6 (microseconds)
    };
    QVector<IfaceInfo> interfaces;

    while (!ds.atEnd())
    {
        qint64 blockStart = file.pos();
        quint32 blockType, blockTotalLen;
        ds >> blockType >> blockTotalLen;
        if (ds.status() != QDataStream::Ok)
            break;
        if (blockTotalLen < 12)
            break;

        if (blockType == BT_SHB)
        {
            // Section Header Block — determine byte order
            quint32 bom;
            ds >> bom;
            if (bom == BYTE_ORDER_MAGIC)
            {
                ds.setByteOrder(QDataStream::LittleEndian);
            }
            else if (bom == 0x4D3C2B1A)
            {
                ds.setByteOrder(QDataStream::BigEndian);
            }
            else
            {
                return false;
            }
            // Re-read blockTotalLen with correct byte order
            file.seek(blockStart + 4);
            ds >> blockTotalLen;
            // Skip rest: version(4) + section_length(8) + options
            interfaces.clear();
        }
        else if (blockType == BT_IDB)
        {
            // Interface Description Block: linktype(2) + reserved(2) + snaplen(4) + options
            quint16 linkType, reserved;
            quint32 snaplen;
            ds >> linkType >> reserved >> snaplen;

            IfaceInfo info;
            info.linkType = linkType;

            // Parse options for if_name (code 2) and if_tsresol (code 9)
            qint64 optStart = file.pos();
            Q_UNUSED(optStart);
            qint64 optEnd = blockStart + blockTotalLen - 4; // exclude trailing total_length
            while (file.pos() + 4 <= optEnd)
            {
                quint16 optCode, optLen;
                ds >> optCode >> optLen;
                if (optCode == 0)
                    break; // opt_endofopt
                qint64 valStart = file.pos();

                if (optCode == 2 && optLen > 0)
                {
                    // if_name
                    QByteArray nameBytes(optLen, 0);
                    ds.readRawData(nameBytes.data(), optLen);
                    // Remove trailing null if present
                    if (nameBytes.endsWith('\0'))
                        nameBytes.chop(1);
                    info.name = QString::fromUtf8(nameBytes);
                }
                else if (optCode == 9 && optLen == 1)
                {
                    // if_tsresol
                    quint8 tsresol;
                    ds >> tsresol;
                    info.tsResol = tsresol;
                }

                // Advance past padded option value
                quint32 paddedLen = (optLen + 3) & ~quint32(3);
                file.seek(valStart + paddedLen);
            }

            if (info.name.isEmpty())
            {
                info.name = QStringLiteral("if%1").arg(interfaces.size());
            }
            interfaces.append(info);
        }
        else if (blockType == BT_EPB)
        {
            // Enhanced Packet Block: interface_id(4) + ts_high(4) + ts_low(4) + captured_len(4) + original_len(4) + data
            quint32 ifaceIdx, tsHigh, tsLow, capturedLen, originalLen;
            ds >> ifaceIdx >> tsHigh >> tsLow >> capturedLen >> originalLen;

            if (ifaceIdx >= static_cast<quint32>(interfaces.size()))
            {
                // Unknown interface, skip
                file.seek(blockStart + blockTotalLen);
                continue;
            }

            const IfaceInfo &iface = interfaces[ifaceIdx];
            if (iface.linkType != LINKTYPE_CAN_SOCKETCAN)
            {
                file.seek(blockStart + blockTotalLen);
                continue;
            }

            // Convert timestamp to microseconds
            uint64_t tsRaw = (static_cast<uint64_t>(tsHigh) << 32) | tsLow;
            int64_t ts_us;
            if (iface.tsResol == 6)
            {
                ts_us = static_cast<int64_t>(tsRaw); // already microseconds
            }
            else if (iface.tsResol == 9)
            {
                ts_us = static_cast<int64_t>(tsRaw / 1000); // nanoseconds
            }
            else if (iface.tsResol == 3)
            {
                ts_us = static_cast<int64_t>(tsRaw * 1000); // milliseconds
            }
            else
            {
                // Generic power-of-10: tsResol is the exponent
                double scale = 1e6;
                for (quint8 e = 0; e < iface.tsResol; e++)
                    scale /= 10.0;
                ts_us = static_cast<int64_t>(tsRaw * scale);
            }

            BusMessage msg;
            bool valid;
            parseSocketCanFrame(ds, capturedLen, msg, valid);

            if (valid)
            {
                msg.setTimestamp_us(ts_us);
                _messages.append(msg);
                _messageInterfaces.append(iface.name);
            }
        }

        // Skip to end of block (blockTotalLen includes type + length fields at start and trailing length)
        file.seek(blockStart + blockTotalLen);
    }

    return !_messages.isEmpty();
}

void ReplayWindow::buildFilterTree()
{
    _filterTree->blockSignals(true);
    _filterTree->clear();
    _enabledDirs.clear();
    _channelCombos.clear();

    struct IdInfo
    {
        int count = 0;
        bool hasRx = false;
        bool hasTx = false;
    };

    // Collect interface -> (id -> info), and which interfaces carry LIN
    QMap<QString, QMap<uint32_t, IdInfo>> ifaceIds;
    QSet<QString> linChannels;
    for (int i = 0; i < _messages.size(); i++)
    {
        const QString &iface = _messageInterfaces[i];
        uint32_t id = _messages[i].isErrorFrame() ? ErrorFrameId : _messages[i].getId();
        IdInfo &info = ifaceIds[iface][id];
        info.count++;
        if (_messages[i].isRX())
            info.hasRx = true;
        else
            info.hasTx = true;
        if (_messages[i].busType() == BusType::LIN)
            linChannels.insert(iface);
    }

    // Collect available CAN interfaces for the combo boxes
    BusInterfaceIdList availableInterfaces = _backend->getInterfaceList();

    // Build tree items
    for (auto ifaceIt = ifaceIds.constBegin(); ifaceIt != ifaceIds.constEnd(); ++ifaceIt)
    {
        const QString &iface = ifaceIt.key();
        const QMap<uint32_t, IdInfo> &ids = ifaceIt.value();

        const bool isLin = linChannels.contains(iface);

        auto *ifaceItem = new QTreeWidgetItem(_filterTree);
        ifaceItem->setText(0, isLin ? tr("%1 (LIN)").arg(iface) : tr("%1 (CAN)").arg(iface));
        ifaceItem->setFlags(ifaceItem->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsAutoTristate);
        ifaceItem->setCheckState(0, Qt::Checked);

        // Output interface combo box — LIN channels are locked to trace-only
        auto *combo = new QComboBox(_filterTree);
        combo->addItem(tr("Trace only"), QVariant(static_cast<int>(-1)));
        if (!isLin)
        {
            for (BusInterfaceId id : availableInterfaces)
            {
                BusInterface *intf = _backend->getInterfaceById(id);
                if (intf && intf->busType() == BusType::LIN)
                    continue;
                combo->addItem(_backend->getInterfaceName(id), QVariant(static_cast<int>(id)));
            }
        }
        else
        {
            combo->setEnabled(false);
            combo->setToolTip(tr("LIN frames can only be replayed to the trace"));
        }
        _channelCombos[iface] = combo;

        int ifaceTotal = 0;
        QMap<uint32_t, uint8_t> enabledMap;

        // Sort IDs numerically
        QList<uint32_t> sortedIds = ids.keys();
        std::sort(sortedIds.begin(), sortedIds.end());

        for (uint32_t id : sortedIds)
        {
            const IdInfo &info = ids[id];
            ifaceTotal += info.count;

            auto *idItem = new QTreeWidgetItem(ifaceItem);

            if (id == ErrorFrameId)
            {
                idItem->setText(0, tr("ERROR"));
            }
            else
            {
                idItem->setText(0, QString("0x%1").arg(id, 0, 16, QChar('0')).toUpper());

                // Look up DBC message name
                BusMessage tmp;
                tmp.setId(id);
                tmp.setExtended(id > 0x7FF);
                CanDbMessage *dbMsg = _backend->findDbMessage(tmp);
                if (dbMsg)
                {
                    idItem->setText(1, dbMsg->getName());
                }
            }

            // RX / TX checkboxes
            idItem->setFlags(idItem->flags() | Qt::ItemIsUserCheckable);
            idItem->setCheckState(0, Qt::Checked);
            uint8_t dirs = 0;
            if (info.hasRx)
            {
                idItem->setCheckState(2, Qt::Checked);
                dirs |= DirRx;
            }
            if (info.hasTx)
            {
                idItem->setCheckState(3, Qt::Checked);
                dirs |= DirTx;
            }

            idItem->setText(4, QString::number(info.count));
            idItem->setTextAlignment(4, Qt::AlignRight | Qt::AlignVCenter);
            idItem->setData(0, Qt::UserRole, id);
            idItem->setData(0, Qt::UserRole + 1, iface);

            enabledMap[id] = dirs;
        }

        ifaceItem->setText(4, QString::number(ifaceTotal));
        ifaceItem->setTextAlignment(4, Qt::AlignRight | Qt::AlignVCenter);
        _enabledDirs[iface] = enabledMap;

        // Set combo box on interface item after it's added to the tree
        _filterTree->setItemWidget(ifaceItem, 5, combo);
    }

    _filterTree->expandAll();
    _filterTree->blockSignals(false);
}

void ReplayWindow::onFilterItemChanged(QTreeWidgetItem *item, int column)
{
    _filterTree->blockSignals(true);

    if (item->childCount() > 0)
    {
        // Interface-level item: propagate column 0 to children
        Qt::CheckState state = item->checkState(0);
        QString iface = item->text(0);
        for (int i = 0; i < item->childCount(); i++)
        {
            item->child(i)->setCheckState(0, state);
        }
        if (state == Qt::Checked)
        {
            for (int i = 0; i < item->childCount(); i++)
            {
                auto *child = item->child(i);
                uint32_t id = child->data(0, Qt::UserRole).toUInt();
                uint8_t dirs = 0;
                if (child->data(2, Qt::CheckStateRole).isValid() && child->checkState(2) == Qt::Checked)
                    dirs |= DirRx;
                if (child->data(3, Qt::CheckStateRole).isValid() && child->checkState(3) == Qt::Checked)
                    dirs |= DirTx;
                _enabledDirs[iface][id] = dirs;
            }
        }
        else
        {
            _enabledDirs[iface].clear();
        }
    }
    else
    {
        // ID-level item
        QString iface = item->data(0, Qt::UserRole + 1).toString();
        uint32_t id = item->data(0, Qt::UserRole).toUInt();

        if (item->checkState(0) == Qt::Checked)
        {
            uint8_t dirs = 0;
            if (item->data(2, Qt::CheckStateRole).isValid() && item->checkState(2) == Qt::Checked)
                dirs |= DirRx;
            if (item->data(3, Qt::CheckStateRole).isValid() && item->checkState(3) == Qt::Checked)
                dirs |= DirTx;
            _enabledDirs[iface][id] = dirs;
        }
        else if (column == 0)
        {
            _enabledDirs[iface].remove(id);
        }
        else
        {
            // RX/TX checkbox changed — update direction flags
            uint8_t dirs = 0;
            if (item->data(2, Qt::CheckStateRole).isValid() && item->checkState(2) == Qt::Checked)
                dirs |= DirRx;
            if (item->data(3, Qt::CheckStateRole).isValid() && item->checkState(3) == Qt::Checked)
                dirs |= DirTx;
            _enabledDirs[iface][id] = dirs;
        }

        // Update parent tri-state
        QTreeWidgetItem *parent = item->parent();
        if (parent)
        {
            int checked = 0, unchecked = 0;
            for (int i = 0; i < parent->childCount(); i++)
            {
                if (parent->child(i)->checkState(0) == Qt::Checked)
                    checked++;
                else
                    unchecked++;
            }
            if (checked == parent->childCount())
                parent->setCheckState(0, Qt::Checked);
            else if (unchecked == parent->childCount())
                parent->setCheckState(0, Qt::Unchecked);
            else
                parent->setCheckState(0, Qt::PartiallyChecked);
        }
    }

    _filterTree->blockSignals(false);
}

void ReplayWindow::onSelectAllClicked()
{
    _filterTree->blockSignals(true);
    for (int i = 0; i < _filterTree->topLevelItemCount(); i++)
    {
        QTreeWidgetItem *ifaceItem = _filterTree->topLevelItem(i);
        ifaceItem->setCheckState(0, Qt::Checked);
        QString iface = ifaceItem->text(0);
        for (int j = 0; j < ifaceItem->childCount(); j++)
        {
            auto *child = ifaceItem->child(j);
            child->setCheckState(0, Qt::Checked);
            uint32_t id = child->data(0, Qt::UserRole).toUInt();
            uint8_t dirs = 0;
            if (child->data(2, Qt::CheckStateRole).isValid())
            {
                child->setCheckState(2, Qt::Checked);
                dirs |= DirRx;
            }
            if (child->data(3, Qt::CheckStateRole).isValid())
            {
                child->setCheckState(3, Qt::Checked);
                dirs |= DirTx;
            }
            _enabledDirs[iface][id] = dirs;
        }
    }
    _filterTree->blockSignals(false);
}

void ReplayWindow::onDeselectAllClicked()
{
    _filterTree->blockSignals(true);
    for (int i = 0; i < _filterTree->topLevelItemCount(); i++)
    {
        QTreeWidgetItem *ifaceItem = _filterTree->topLevelItem(i);
        ifaceItem->setCheckState(0, Qt::Unchecked);
        QString iface = ifaceItem->text(0);
        _enabledDirs[iface].clear();
        for (int j = 0; j < ifaceItem->childCount(); j++)
        {
            auto *child = ifaceItem->child(j);
            child->setCheckState(0, Qt::Unchecked);
            if (child->data(2, Qt::CheckStateRole).isValid())
                child->setCheckState(2, Qt::Unchecked);
            if (child->data(3, Qt::CheckStateRole).isValid())
                child->setCheckState(3, Qt::Unchecked);
        }
    }
    _filterTree->blockSignals(false);
}

bool ReplayWindow::isMessageEnabled(int index) const
{
    const QString &iface = _messageInterfaces[index];
    uint32_t id = _messages[index].isErrorFrame() ? ErrorFrameId : _messages[index].getId();
    auto ifaceIt = _enabledDirs.constFind(iface);
    if (ifaceIt == _enabledDirs.constEnd())
        return false;
    auto idIt = ifaceIt->constFind(id);
    if (idIt == ifaceIt->constEnd())
        return false;
    uint8_t flag = _messages[index].isRX() ? DirRx : DirTx;
    return (*idIt & flag) != 0;
}

BusInterfaceId ReplayWindow::getMappedInterface(const QString &channel) const
{
    auto it = _channelCombos.constFind(channel);
    if (it == _channelCombos.constEnd())
    {
        return -1;
    }
    return static_cast<BusInterfaceId>(it.value()->currentData().toInt());
}

void ReplayWindow::onPlayClicked()
{
    if (_messages.isEmpty())
    {
        return;
    }

    _playing = true;
    _btnPlay->setEnabled(false);
    _btnStop->setEnabled(true);
    _btnLoad->setEnabled(false);
    _slider->setEnabled(false);
    _playbackIndex = 0;
    _slider->setValue(0);

    _traceStartTime = _messages[_playbackIndex].getFloatTimestamp();
    _elapsed.start();

    _timer->start(1);
}

void ReplayWindow::onStopClicked()
{
    _playing = false;
    _timer->stop();
    _btnPlay->setEnabled(true);
    _btnStop->setEnabled(false);
    _btnLoad->setEnabled(true);
    _slider->setEnabled(true);
}

void ReplayWindow::onBeginMeasurement()
{
    if (_cbAutoplay->isChecked() && !_messages.isEmpty() && !_playing)
    {
        _playbackIndex = 0;
        _slider->setValue(0);
        onPlayClicked();
    }
}

void ReplayWindow::onEndMeasurement()
{
    if (_cbAutoplay->isChecked() && _playing)
    {
        onStopClicked();
        _playbackIndex = 0;
        _slider->setValue(0);
        updatePositionLabel();
    }
}

void ReplayWindow::onSliderMoved(int value)
{
    _playbackIndex = value;
    updatePositionLabel();
}

void ReplayWindow::onTimerTick()
{
    if (!_playing || _playbackIndex >= _messages.size())
    {
        onStopClicked();
        return;
    }

    BusTrace *trace = _backend->getTrace();
    double speed = _speedSpin->value();

    // How far into the trace we should be, based on wall-clock time and speed
    double wallElapsed = _elapsed.nsecsElapsed() / 1.0e9;
    double traceDeadline = _traceStartTime + wallElapsed * speed;

    // Send all messages whose original timestamp <= deadline
    while (_playbackIndex < _messages.size())
    {
        double msgTime = _messages[_playbackIndex].getFloatTimestamp();
        if (msgTime > traceDeadline)
        {
            break;
        }

        if (isMessageEnabled(_playbackIndex))
        {
            BusMessage msg = _messages[_playbackIndex];
            msg.setTimestamp(_backend->currentTimeStamp());

            const QString &channel = _messageInterfaces[_playbackIndex];
            int mappedInt = _channelCombos.contains(channel) ? _channelCombos[channel]->currentData().toInt() : -1;
            BusInterface *intf = (mappedInt >= 0) ? _backend->getInterfaceById(static_cast<BusInterfaceId>(mappedInt)) : nullptr;

            bool sentOnInterface = false;
            if (intf && intf->isOpen() && !msg.isErrorFrame() && msg.busType() != BusType::LIN)
            {
                msg.setInterfaceId(static_cast<BusInterfaceId>(mappedInt));
                msg.setRX(false);
                intf->sendMessage(msg);
                sentOnInterface = true;
            }

            if (!sentOnInterface)
            {
                if (mappedInt >= 0)
                {
                    msg.setInterfaceId(static_cast<BusInterfaceId>(mappedInt));
                }

                bool moreToFollow = false;
                for (int next = _playbackIndex + 1; next < _messages.size(); next++)
                {
                    if (_messages[next].getFloatTimestamp() > traceDeadline)
                    {
                        break;
                    }
                    if (isMessageEnabled(next))
                    {
                        moreToFollow = true;
                        break;
                    }
                }

                trace->enqueueMessage(msg, moreToFollow);
            }
        }
        _playbackIndex++;
    }

    _slider->setValue(_playbackIndex);
    updatePositionLabel();

    if (_playbackIndex >= _messages.size())
    {
        if (_cbLoop->isChecked())
        {
            _playbackIndex = 0;
            _slider->setValue(0);
            _traceStartTime = _messages[0].getFloatTimestamp();
            _elapsed.restart();
        }
        else
        {
            onStopClicked();
        }
    }
}

void ReplayWindow::updatePositionLabel()
{
    _posLabel->setText(QString("%1 / %2").arg(_playbackIndex).arg(_messages.size()));
    int pct = _messages.isEmpty() ? 0 : static_cast<int>(_playbackIndex * 100 / _messages.size());
    _progressBar->setValue(pct);
}

bool ReplayWindow::saveXML(Backend &backend, QDomDocument &xml, QDomElement &root)
{
    (void)backend;
    root.setAttribute("file", _traceFilePath);
    root.setAttribute("autoplay", _cbAutoplay->isChecked() ? "1" : "0");
    root.setAttribute("loop", _cbLoop->isChecked() ? "1" : "0");
    root.setAttribute("speed", QString::number(_speedSpin->value()));

    // Save filter states: interface -> (id -> dirFlags)
    for (auto ifaceIt = _enabledDirs.constBegin(); ifaceIt != _enabledDirs.constEnd(); ++ifaceIt)
    {
        QDomElement ifaceEl = xml.createElement("filter");
        ifaceEl.setAttribute("channel", ifaceIt.key());

        // Save channel mapping combo selection
        if (_channelCombos.contains(ifaceIt.key()))
        {
            QComboBox *combo = _channelCombos[ifaceIt.key()];
            ifaceEl.setAttribute("mappedInterface", combo->currentText());
        }

        const QMap<uint32_t, uint8_t> &ids = ifaceIt.value();
        for (auto idIt = ids.constBegin(); idIt != ids.constEnd(); ++idIt)
        {
            QDomElement idEl = xml.createElement("id");
            idEl.setAttribute("value", QString::number(idIt.key()));
            idEl.setAttribute("dirs", QString::number(idIt.value()));
            ifaceEl.appendChild(idEl);
        }

        root.appendChild(ifaceEl);
    }

    return true;
}

bool ReplayWindow::loadXML(Backend &backend, QDomElement &el)
{
    (void)backend;
    _cbAutoplay->setChecked(el.attribute("autoplay", "0") == "1");
    _cbLoop->setChecked(el.attribute("loop", "0") == "1");
    _speedSpin->setValue(el.attribute("speed", "1.0").toDouble());

    QString filepath = el.attribute("file");
    if (!filepath.isEmpty())
    {
        loadTraceFile(filepath);
    }

    // Restore filter states and channel mappings
    QDomNodeList filters = el.elementsByTagName("filter");
    for (int i = 0; i < filters.size(); i++)
    {
        QDomElement filterEl = filters.at(i).toElement();
        QString channel = filterEl.attribute("channel");

        // Restore channel mapping
        if (_channelCombos.contains(channel))
        {
            QString mappedName = filterEl.attribute("mappedInterface");
            QComboBox *combo = _channelCombos[channel];
            int idx = combo->findText(mappedName);
            if (idx >= 0)
            {
                combo->setCurrentIndex(idx);
            }
        }

        // Restore filter dir flags
        if (_enabledDirs.contains(channel))
        {
            QDomNodeList idNodes = filterEl.elementsByTagName("id");
            for (int j = 0; j < idNodes.size(); j++)
            {
                QDomElement idEl = idNodes.at(j).toElement();
                uint32_t id = idEl.attribute("value").toUInt();
                uint8_t dirs = static_cast<uint8_t>(idEl.attribute("dirs").toUInt());

                if (_enabledDirs[channel].contains(id))
                {
                    _enabledDirs[channel][id] = dirs;
                }
            }

            // Update tree checkboxes to match restored state
            for (int t = 0; t < _filterTree->topLevelItemCount(); t++)
            {
                QTreeWidgetItem *ifaceItem = _filterTree->topLevelItem(t);
                if (ifaceItem->text(0) != channel)
                {
                    continue;
                }

                for (int c = 0; c < ifaceItem->childCount(); c++)
                {
                    QTreeWidgetItem *idItem = ifaceItem->child(c);
                    uint32_t id = idItem->data(0, Qt::UserRole).toUInt();
                    if (!_enabledDirs[channel].contains(id))
                    {
                        continue;
                    }

                    uint8_t dirs = _enabledDirs[channel][id];
                    _filterTree->blockSignals(true);
                    if (idItem->checkState(2) != Qt::Unchecked)
                        idItem->setCheckState(2, (dirs & DirRx) ? Qt::Checked : Qt::Unchecked);
                    if (idItem->checkState(3) != Qt::Unchecked)
                        idItem->setCheckState(3, (dirs & DirTx) ? Qt::Checked : Qt::Unchecked);

                    // Update main checkbox: unchecked if no dirs enabled
                    bool anyEnabled = (dirs & (DirRx | DirTx)) != 0;
                    idItem->setCheckState(0, anyEnabled ? Qt::Checked : Qt::Unchecked);
                    _filterTree->blockSignals(false);
                }
            }
        }
    }

    return true;
}
