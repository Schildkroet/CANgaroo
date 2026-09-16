/*

  Copyright (c) 2015, 2016 Hubert Denkmair
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

#include "SocketCanInterface.h"

#include <chrono>
#include <cstring>

#include <QMutexLocker>
#include <QProcess>
#include <QStandardPaths>
#include <QString>
#include <QStringList>

#include "core/Backend.h"
#include "core/BusMessage.h"
#include "core/MeasurementInterface.h"
#include "core/MeasurementNetwork.h"

#include <unistd.h>
#include <sys/utsname.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <linux/can.h>
#include <linux/can/netlink.h>
#include <linux/can/raw.h>
#include <linux/if.h>
#include <linux/sockios.h>
#include <netlink/route/link.h>
#include <netlink/route/link/can.h>
#include <netlink/version.h>


SocketCanInterface::SocketCanInterface(SocketCanDriver *driver, int index, QString name)
    : BusInterface(reinterpret_cast<CanDriver *>(driver)),
      _idx(index),
      _isOpen(false),
      _fd(0),
      _name(name),
      _ts_mode(ts_mode_SIOCSHWTSTAMP)
{
    _status.rx_count.store(0);
    _status.rx_errors.store(0);
    _status.rx_overruns.store(0);
    _status.tx_count.store(0);
    _status.tx_errors.store(0);
    _status.tx_dropped.store(0);

    memset(&_config, 0, sizeof(_config));
    _offset_stats = {};

    _ts_mode = ts_mode_SIOCGSTAMP;
}

SocketCanInterface::~SocketCanInterface()
{
}

QString SocketCanInterface::getName() const
{
    return _name;
}

QString SocketCanInterface::getVersion()
{
    struct utsname uts;
    if (uname(&uts) == 0)
    {
        return QString("%1").arg(uts.release);
    }
    return BusInterface::getVersion();
}

void SocketCanInterface::setName(QString name)
{
    _name = name;
}

QList<CanTiming> SocketCanInterface::getAvailableBitrates()
{
    QList<CanTiming> retval;
    QList<unsigned> bitrates({10000, 20000, 50000, 83333, 100000, 125000, 250000, 500000, 800000, 1000000});
    QList<unsigned> samplePoints({500, 625, 750, 875});

    unsigned i = 0;
    for (unsigned br : bitrates)
    {
        for (unsigned sp : samplePoints)
        {
            retval << CanTiming(i++, br, 0, sp);
        }
    }

    // Add CAN FD Data Bitrates if supported
    if (supportsCanFD())
    {
        QList<unsigned> fdBitrates({500000, 1000000, 2000000, 4000000, 5000000, 8000000});
        for (unsigned br : bitrates)
        {
            for (unsigned fdbr : fdBitrates)
            {
                // For simplicity, we add FD variants of common arbitration bitrates
                retval << CanTiming(i++, br, fdbr, 800, 800);
            }
        }
    }

    return retval;
}

QString SocketCanInterface::buildIpRouteCmd(const MeasurementInterface &mi)
{
    QStringList cmd;
    cmd.append("ip");
    cmd.append("link");
    cmd.append("set");
    cmd.append(getName());
    cmd.append("up");
    cmd.append("type");
    cmd.append("can");

    cmd.append("bitrate");
    cmd.append(QString().number(mi.bitrate()));
    cmd.append("sample-point");
    cmd.append(QString().number(static_cast<float>(mi.samplePoint()) / 1000.0, 'f', 3));

    if (mi.isCanFD())
    {
        cmd.append("dbitrate");
        cmd.append(QString().number(mi.fdBitrate()));
        cmd.append("dsample-point");
        cmd.append(QString().number(static_cast<float>(mi.fdSamplePoint()) / 1000.0, 'f', 3));
        cmd.append("fd");
        cmd.append("on");
    }

    cmd.append("restart-ms");
    if (mi.doAutoRestart())
    {
        cmd.append(QString().number(mi.autoRestartMs()));
    }
    else
    {
        cmd.append("0");
    }

    return cmd.join(' ');
}

void SocketCanInterface::applyConfig(const MeasurementInterface &mi)
{
    if (!mi.doConfigure())
    {
        log_info(QString("interface %1 not managed by cangaroo, not touching configuration").arg(getName()));
        return;
    }

    log_info(QString("reconfiguring interface %1").arg(getName()));

    QString ipExe = QStandardPaths::findExecutable("ip");
    if (ipExe.isEmpty())
        ipExe = "/sbin/ip";

    bool needElevation = (geteuid() != 0);
    if (needElevation)
        log_info("Not running as root — using pkexec for ip link commands");

    auto runIp = [&](const QStringList &args) -> bool
    {
        QProcess proc;
        if (needElevation)
            proc.start("pkexec", QStringList{ipExe} + args);
        else
            proc.start(ipExe, args);

        if (!proc.waitForFinished(10000))
        {
            log_error("timeout waiting for ip command");
            return false;
        }
        if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0)
        {
            log_error(QString("ip command failed: ") + QString(proc.readAllStandardError()).trimmed());
            return false;
        }
        return true;
    };

    runIp({"link", "set", getName(), "down"});

    QStringList upArgs;
    upArgs << "link" << "set" << getName() << "up" << "type" << "can";
    upArgs << "bitrate" << QString::number(mi.bitrate());
    upArgs << "sample-point" << QString::number(static_cast<float>(mi.samplePoint()) / 1000.0f, 'f', 3);

    if (mi.isCanFD())
    {
        upArgs << "dbitrate" << QString::number(mi.fdBitrate());
        upArgs << "dsample-point" << QString::number(static_cast<float>(mi.fdSamplePoint()) / 1000.0f, 'f', 3);
        upArgs << "fd" << "on";
    }

    upArgs << "restart-ms" << (mi.doAutoRestart() ? QString::number(mi.autoRestartMs()) : "0");

    log_info(ipExe + " " + upArgs.join(" "));
    runIp(upArgs);
}

#include <linux/netlink.h>
#include <linux/rtnetlink.h>

// Read CAN state directly via a raw RTNETLINK RTM_GETLINK request.
// Used as the primary implementation on libnl < 3.2.22 and as a fallback
// on newer libnl when rtnl_link_can_state() returns an error.
static int can_state_from_rtnetlink(int ifindex, uint32_t *state)
{
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0)
        return -1;

    struct
    {
        struct nlmsghdr hdr;
        struct ifinfomsg ifi;
    } req{};
    req.hdr.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
    req.hdr.nlmsg_type = RTM_GETLINK;
    req.hdr.nlmsg_flags = NLM_F_REQUEST;
    req.hdr.nlmsg_seq = 1;
    req.ifi.ifi_index = ifindex;

    if (send(fd, &req, req.hdr.nlmsg_len, 0) < 0)
    {
        close(fd);
        return -1;
    }

    char buf[16384];
    ssize_t len = recv(fd, buf, sizeof(buf), 0);
    close(fd);
    if (len < 0)
        return -1;

    for (auto *nlh = reinterpret_cast<struct nlmsghdr *>(buf);
         NLMSG_OK(nlh, static_cast<uint32_t>(len));
         nlh = NLMSG_NEXT(nlh, len))
    {
        if (nlh->nlmsg_type != RTM_NEWLINK)
            continue;

        auto *ifi = reinterpret_cast<struct ifinfomsg *>(NLMSG_DATA(nlh));
        if (ifi->ifi_index != ifindex)
            continue;

        auto *rta = IFLA_RTA(ifi);
        int rta_len = static_cast<int>(IFLA_PAYLOAD(nlh));
        for (; RTA_OK(rta, rta_len); rta = RTA_NEXT(rta, rta_len))
        {
            if (rta->rta_type != IFLA_LINKINFO)
                continue;

            auto *info = reinterpret_cast<struct rtattr *>(RTA_DATA(rta));
            int info_len = static_cast<int>(RTA_PAYLOAD(rta));
            for (; RTA_OK(info, info_len); info = RTA_NEXT(info, info_len))
            {
                if (info->rta_type != IFLA_INFO_DATA)
                    continue;

                auto *ca = reinterpret_cast<struct rtattr *>(RTA_DATA(info));
                int ca_len = static_cast<int>(RTA_PAYLOAD(info));
                for (; RTA_OK(ca, ca_len); ca = RTA_NEXT(ca, ca_len))
                {
                    if (ca->rta_type != IFLA_CAN_STATE)
                        continue;
                    *state = *reinterpret_cast<uint32_t *>(RTA_DATA(ca));
                    return 0;
                }
            }
        }
    }

    return -1;
}

#if (LIBNL_CURRENT <= 216)
#warning libnl3 < 3.2.22 detected - using raw RTNETLINK fallback for rtnl_link_can_state
int rtnl_link_can_state(struct rtnl_link *link, uint32_t *state)
{
    return can_state_from_rtnetlink(rtnl_link_get_ifindex(link), state);
}
#endif

bool SocketCanInterface::updateStatus()
{
    bool retval = false;

    struct nl_sock *sock = nl_socket_alloc();
    struct nl_cache *cache = nullptr;
    struct rtnl_link *link;
    uint32_t state;

    _status.can_state.store(state_unknown);

    nl_connect(sock, NETLINK_ROUTE);
    if (rtnl_link_alloc_cache(sock, AF_UNSPEC, &cache) >= 0)
    {
        if (rtnl_link_get_kernel(sock, _idx, 0, &link) == 0)
        {

            _status.rx_count.store(rtnl_link_get_stat(link, RTNL_LINK_RX_PACKETS));
            _status.rx_overruns.store(rtnl_link_get_stat(link, RTNL_LINK_RX_OVER_ERR));
            _status.tx_count.store(rtnl_link_get_stat(link, RTNL_LINK_TX_PACKETS));
            _status.tx_dropped.store(rtnl_link_get_stat(link, RTNL_LINK_TX_DROPPED));

            if (rtnl_link_is_can(link))
            {
                if (rtnl_link_can_state(link, &state) == 0 || can_state_from_rtnetlink(rtnl_link_get_ifindex(link), &state) == 0)
                {
                    _status.can_state.store(state);
                }
                _status.rx_errors.store(rtnl_link_can_berr_rx(link));
                _status.tx_errors.store(rtnl_link_can_berr_tx(link));
            }
            else
            {
                const char *type = rtnl_link_get_type(link);
                if (type && strcmp(type, "vcan") == 0)
                {
                    _status.can_state.store(state_ok);
                }
                _status.rx_errors.store(0);
                _status.tx_errors.store(0);
            }
            retval = true;
        }
    }

    if (cache)
    {
        nl_cache_free(cache);
    }
    nl_close(sock);
    nl_socket_free(sock);

    return retval;
}

bool SocketCanInterface::readConfig()
{
    bool retval = false;

    struct nl_sock *sock = nl_socket_alloc();
    struct nl_cache *cache = nullptr;
    struct rtnl_link *link;

    nl_connect(sock, NETLINK_ROUTE);
    int result = rtnl_link_alloc_cache(sock, AF_UNSPEC, &cache);

    if (result >= 0)
    {
        if (rtnl_link_get_kernel(sock, _idx, 0, &link) == 0)
        {
            retval = readConfigFromLink(link);
        }
    }

    if (cache)
    {
        nl_cache_free(cache);
    }
    nl_close(sock);
    nl_socket_free(sock);

    return retval;
}

bool SocketCanInterface::readConfigFromLink(rtnl_link *link)
{
    _config.state = state_unknown;
    _config.supports_canfd = (rtnl_link_get_mtu(link) == 72);
    _config.supports_timing = rtnl_link_is_can(link);
    if (_config.supports_timing)
    {
        rtnl_link_can_freq(link, &_config.base_freq);
        rtnl_link_can_get_ctrlmode(link, &_config.ctrl_mode);
        rtnl_link_can_get_bittiming(link, &_config.bit_timing);
        rtnl_link_can_get_sample_point(link, &_config.sample_point);
        rtnl_link_can_get_restart_ms(link, &_config.restart_ms);
    }
    else
    {
        const char *type = rtnl_link_get_type(link);
        if (type && strcmp(type, "vcan") == 0)
        {
            _config.state = state_ok;
            _config.supports_canfd = true;
            _config.supports_timing = false;
            memset(&_config.bit_timing, 0, sizeof(_config.bit_timing));
        }
    }
    return true;
}

bool SocketCanInterface::supportsTimingConfiguration()
{
    return _config.supports_timing;
}

bool SocketCanInterface::supportsCanFD()
{
    return _config.supports_canfd;
}

bool SocketCanInterface::supportsTripleSampling()
{
    return false;
}

unsigned SocketCanInterface::getBitrate()
{
    unsigned br = 0;
    if (readConfig())
    {
        br = _config.bit_timing.bitrate;
    }

    if (br != 0)
    {
        _bitrateFallbackLogged = false;
    }
    else
    {
        // Fallback to setup bitrate
        for (auto *network : Backend::instance().getSetup().getNetworks())
        {
            for (auto *mi : network->interfaces())
            {
                if (mi->busInterface() == getId())
                {
                    unsigned fallbackBr = mi->bitrate();
                    // Polled periodically (bus load), so only report the fallback once
                    // until the kernel reports a real bitrate again
                    if (!_name.startsWith("vcan") && !_bitrateFallbackLogged)
                    {
                        _bitrateFallbackLogged = true;
                        log_info(QString("SocketCanInterface %1: getBitrate() fallback to %2 (ID match %3)").arg(_name).arg(fallbackBr).arg(getId()));
                    }
                    return fallbackBr;
                }
            }
        }
    }

    if (br == 0)
        br = 500000; // Final safety fallback

    return br;
}

uint32_t SocketCanInterface::getCapabilities()
{
    uint32_t retval =
        BusInterface::capability_config_os |
        BusInterface::capability_listen_only |
        BusInterface::capability_auto_restart;

    if (supportsCanFD())
    {
        retval |= BusInterface::capability_canfd;
    }

    if (supportsTripleSampling())
    {
        retval |= BusInterface::capability_triple_sampling;
    }

    return retval;
}

bool SocketCanInterface::updateStatistics()
{
    return updateStatus();
}

void SocketCanInterface::resetStatistics()
{
    // Snapshot current atomic stats into the plain offset struct
    _offset_stats.can_state = _status.can_state.load();
    _offset_stats.rx_count = _status.rx_count.load();
    _offset_stats.rx_errors = _status.rx_errors.load();
    _offset_stats.rx_overruns = _status.rx_overruns.load();
    _offset_stats.tx_count = _status.tx_count.load();
    _offset_stats.tx_errors = _status.tx_errors.load();
    _offset_stats.tx_dropped = _status.tx_dropped.load();
    BusInterface::resetStatistics();
}

uint32_t SocketCanInterface::getState()
{
    switch (_status.can_state.load())
    {
    case CAN_STATE_ERROR_ACTIVE:
        return state_ok;
    case CAN_STATE_ERROR_WARNING:
        return state_warning;
    case CAN_STATE_ERROR_PASSIVE:
        return state_passive;
    case CAN_STATE_BUS_OFF:
        return state_bus_off;
    case CAN_STATE_STOPPED:
        return state_stopped;
    default:
        return state_unknown;
    }
}

int SocketCanInterface::getNumRxFrames()
{
    return static_cast<int>(_status.rx_count.load() - _offset_stats.rx_count);
}

int SocketCanInterface::getNumRxErrors()
{
    return static_cast<int>(_status.rx_errors.load() - _offset_stats.rx_errors);
}

int SocketCanInterface::getNumTxFrames()
{
    return static_cast<int>(_status.tx_count.load() - _offset_stats.tx_count);
}

int SocketCanInterface::getNumTxErrors()
{
    return static_cast<int>(_status.tx_errors.load() - _offset_stats.tx_errors);
}

int SocketCanInterface::getNumRxOverruns()
{
    return static_cast<int>(_status.rx_overruns.load() - _offset_stats.rx_overruns);
}

int SocketCanInterface::getNumTxDropped()
{
    return static_cast<int>(_status.tx_dropped.load() - _offset_stats.tx_dropped);
}

int SocketCanInterface::getIfIndex()
{
    return _idx;
}

const char *SocketCanInterface::cname()
{
    _cnameBuffer = _name.toStdString();
    return _cnameBuffer.c_str();
}

void SocketCanInterface::open()
{
    // create socket before modifying shared _fd/_isOpen
    int local_fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (local_fd < 0)
    {
        log_error(QString("SocketCanInterface: Error while opening socket: %1").arg(strerror(errno)));
        return;
    }

    struct ifreq ifr;
    struct sockaddr_can addr;

    strncpy(ifr.ifr_name, _name.toStdString().c_str(), IFNAMSIZ - 1);
    ifr.ifr_name[IFNAMSIZ - 1] = '\0';
    if (ioctl(local_fd, SIOCGIFINDEX, &ifr) < 0)
    {
        log_error(QString("SocketCanInterface: Error getting interface index for %1: %2").arg(_name, strerror(errno)));
        ::close(local_fd);
        return;
    }

    addr.can_family = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;

    if (bind(local_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        log_error(QString("SocketCanInterface: Error in socket bind for %1: %2").arg(_name, strerror(errno)));
        ::close(local_fd);
        return;
    }

    int enable = 1;
    if (setsockopt(local_fd, SOL_CAN_RAW, CAN_RAW_FD_FRAMES, &enable, sizeof(enable)) != 0) {
        log_warning(QString("SocketCanInterface: Error while enabling CAN FD support for %1: %2").arg(_name, strerror(errno)));
    }

    // CAN_RAW_ERR_FILTER defaults to 0, i.e. the socket receives no error frames
    // at all. Subscribe to every error class so bus-off, ACK, bit/form/stuff and
    // controller errors reach readMessage() -- an analyzer that silently hides
    // them is worse than useless when a bus goes down.
    const can_err_mask_t errMask = CAN_ERR_MASK;
    if (setsockopt(local_fd, SOL_CAN_RAW, CAN_RAW_ERR_FILTER, &errMask, sizeof(errMask)) != 0) {
        log_warning(QString("SocketCanInterface: Error while enabling error frame reception for %1: %2").arg(_name, strerror(errno)));
    }

    // publish fd and open state under lock
    {
        QMutexLocker fdLock(&_fdMutex);
        _fd = local_fd;
        _isOpen = true;
    }
    QMutexLocker lock(&_txMutex);
    txMsgList.clear();
}

bool SocketCanInterface::isOpen()
{
    QMutexLocker fdLock(&_fdMutex);
    return _isOpen;
}

void SocketCanInterface::close()
{
    QMutexLocker fdLock(&_fdMutex);
    if (_fd >= 0)
    {
        ::close(_fd);
        _fd = -1;
    }
    _isOpen = false;
}

void SocketCanInterface::sendMessage(const BusMessage &msg)
{
    int local_fd;
    {
        QMutexLocker fdLock(&_fdMutex);
        if (!_isOpen)
        {
            log_error(QString("SocketCanInterface: Cannot send message, interface %1 is not open").arg(_name));
            return;
        }
        local_fd = _fd;
    }

    if (msg.isFD() || (supportsCanFD() && msg.getLength() > 8))
    {
        struct canfd_frame frame;
        memset(&frame, 0, sizeof(frame));
        frame.can_id = msg.getId();

        if (msg.isExtended())
        {
            frame.can_id |= CAN_EFF_FLAG;
        }

        if (msg.isErrorFrame())
        {
            frame.can_id |= CAN_ERR_FLAG;
        }

        if (msg.isBRS())
        {
            frame.flags |= CANFD_BRS;
        }

        uint8_t len = msg.getLength();
        if (len > 64)
            len = 64;
        frame.len = len;

        for (int i = 0; i < len; i++)
        {
            frame.data[i] = msg.getByte(i);
        }

        if (::write(local_fd, &frame, sizeof(struct canfd_frame)) < 0)
        {
            log_error(QString("SocketCanInterface: Error writing FD frame to %1: %2").arg(_name, strerror(errno)));
            return;
        }
    }
    else
    {
        struct can_frame frame;
        memset(&frame, 0, sizeof(frame));
        frame.can_id = msg.getId();

        if (msg.isExtended())
        {
            frame.can_id |= CAN_EFF_FLAG;
        }

        if (msg.isRTR())
        {
            frame.can_id |= CAN_RTR_FLAG;
        }

        if (msg.isErrorFrame())
        {
            frame.can_id |= CAN_ERR_FLAG;
        }

        uint8_t len = msg.getLength();
        if (len > 8)
            len = 8;
        frame.can_dlc = len;

        if (!msg.isRTR())
        {
            for (int i = 0; i < len; i++)
            {
                frame.data[i] = msg.getByte(i);
            }
        }

        if (::write(local_fd, &frame, sizeof(struct can_frame)) < 0)
        {
            log_error(QString("SocketCanInterface: Error writing frame to %1: %2").arg(_name, strerror(errno)));
            return;
        }
    }

    // Only reached on successful write
    addFrameBits(msg);

    BusMessage txMsg = msg;
    txMsg.setRX(false);
    auto now = std::chrono::system_clock::now().time_since_epoch();
    txMsg.setTimestamp_us(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
    QMutexLocker lock(&_txMutex);
    txMsgList.append(txMsg);
}

bool SocketCanInterface::readMessage(QList<BusMessage> &msglist, unsigned int timeout_ms)
{
    struct canfd_frame frame;
    struct timespec ts_rcv;
    struct timeval tv_rcv;
    struct timeval timeout;
    fd_set fdset;

    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = 1000 * (timeout_ms % 1000);

    // Snapshot fd under lock to avoid races with close()
    int local_fd;
    {
        QMutexLocker fdLock(&_fdMutex);
        local_fd = _fd;
    }

    if (local_fd < 0)
        return !msglist.isEmpty();

    FD_ZERO(&fdset);
    FD_SET(local_fd, &fdset);

    // Enqueue tx messages
    {
        QMutexLocker lock(&_txMutex);
        msglist.append(txMsgList);
        txMsgList.clear();
    }

    if (!msglist.isEmpty())
    {
        timeout.tv_sec = 0;
        timeout.tv_usec = 0;
    }

    BusMessage msg;

    for (int retry = 0; retry < 4; retry++)
    {
        int rv = select(local_fd + 1, &fdset, nullptr, nullptr, &timeout);
        if (rv <= 0)
        {
            break;
        }

        int nbytes = ::read(local_fd, &frame, sizeof(struct canfd_frame));
        if (nbytes < 0)
        {
            break;
        }

        if (_ts_mode == ts_mode_SIOCSHWTSTAMP)
        {
            // TODO implement me
            _ts_mode = ts_mode_SIOCGSTAMPNS;
        }

        if (_ts_mode == ts_mode_SIOCGSTAMPNS)
        {
            if (ioctl(local_fd, SIOCGSTAMPNS, &ts_rcv) == 0)
            {
                msg.setTimestamp(ts_rcv.tv_sec, ts_rcv.tv_nsec / 1000);
            }
            else
            {
                _ts_mode = ts_mode_SIOCGSTAMP;
            }
        }

        if (_ts_mode == ts_mode_SIOCGSTAMP)
        {
            if (ioctl(local_fd, SIOCGSTAMP, &tv_rcv) == 0)
            {
                msg.setTimestamp(tv_rcv.tv_sec, tv_rcv.tv_usec);
            }
        }

        msg.setId(frame.can_id & CAN_EFF_MASK);
        msg.setExtended((frame.can_id & CAN_EFF_FLAG) != 0);
        msg.setRTR((frame.can_id & CAN_RTR_FLAG) != 0);
        msg.setInterfaceId(getId());

        if (frame.can_id & CAN_ERR_FLAG) {
            const uint32_t errId = frame.can_id & ~static_cast<uint32_t>(CAN_ERR_FLAG);
            // The ID field carries error classes, not a CAN ID: don't show or
            // decode it as one.
            msg.setId(0);
            msg.setExtended(false);
            if (errId & 0x00000001) msg.setErrorFlag(BusError::TxTimeout);
            if (errId & 0x00000020) msg.setErrorFlag(BusError::Ack);
            if (errId & 0x00000040) msg.setErrorFlag(BusError::BusOff);
            if (errId & 0x00000100) msg.setErrorFlag(BusError::Restarted);
            if (errId & 0x00000004) {
                if (frame.data[1] & 0x03) msg.setErrorFlag(BusError::Overrun);
                if (frame.data[1] & 0x0C) msg.setErrorFlag(BusError::ErrorWarning);
                if (frame.data[1] & 0x30) msg.setErrorFlag(BusError::ErrorPassive);
                if (frame.data[1] & 0x40) msg.setErrorFlag(BusError::ErrorActive);
            }
            if (errId & 0x00000008) {
                const uint8_t prot = static_cast<uint8_t>(frame.data[2]);
                if (prot & 0x01) msg.setErrorFlag(BusError::Bit);
                if (prot & 0x02) msg.setErrorFlag(BusError::Form);
                if (prot & 0x04) msg.setErrorFlag(BusError::Stuff);
                if (prot & 0x18) msg.setErrorFlag(BusError::Bit);
                if (frame.data[3] == 0x08) msg.setErrorFlag(BusError::Crc);
            }
            // CAN_ERR_BUSERROR (0x80) or anything unclassified falls through to
            // the Generic flag below.
            if (!msg.isErrorFrame()) msg.setErrorFlag(BusError::Generic);
        }

        bool isFD = (nbytes == CANFD_MTU);
        msg.setFD(isFD);
        if (isFD)
        {
            msg.setBRS((frame.flags & CANFD_BRS) != 0);
        }

        uint8_t len = frame.len;
        msg.setLength(len);
        for (int i = 0; i < len; i++)
        {
            msg.setByte(i, frame.data[i]);
        }

        msglist.append(msg);
        addFrameBits(msg); // Track received bits

        // Prepare for next iteration with zero timeout (non-blocking check)
        timeout.tv_sec = 0;
        timeout.tv_usec = 0;
        FD_ZERO(&fdset);
        FD_SET(local_fd, &fdset);
    }

    return !msglist.empty();
}
