#ifndef CANTIMING_H
#define CANTIMING_H

#include <QString>

class CanTiming
{
public:
    /*!
     * \brief CanTiming generic CAN timing description
     * \param id internal id to identify this timing for the corresponding CAN driver
     * \param bitrate bitrate in bits per second, e.g. 500000
     * \param bitrate_fd bitrate for the FD part, or zero this is not a CanFD timing
     * \param samplePoint sample point, e.g. 0.875
     */
    CanTiming(unsigned id, unsigned bitrate, unsigned bitrate_fd, unsigned samplePoint);
    CanTiming(unsigned id, unsigned bitrate, unsigned bitrate_fd, unsigned samplePoint, unsigned samplePoint_fd);
    unsigned getId() const;
    unsigned getBitrate() const;
    unsigned getBitrateFD() const;
    bool isCanFD() const;
    unsigned getSamplePoint() const;
    static QString getSamplePointStr(unsigned samplePoint);
    unsigned getSamplePointFD() const;
    static QString getSamplePointFDStr(unsigned samplePoint);

    /*!
     * \brief getTitle get a brief one-line description of this timing
     * \return description, e.g. "500kBit/s, sample point 87.5%"
     */
    QString getTitle() const;

private:
    unsigned _id;
    unsigned _bitrate;
    unsigned _bitrate_fd;
    unsigned _samplePoint;
    unsigned _samplePoint_fd;
};

#endif // CANTIMING_H
