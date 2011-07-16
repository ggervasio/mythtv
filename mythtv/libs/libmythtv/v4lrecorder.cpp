// -*- Mode: c++ -*-

#ifdef USING_V4L1
#include <linux/videodev.h> // for vbi_format
#endif // USING_V4L1

#ifdef USING_V4L2
#include <linux/videodev2.h>
#endif // USING_V4L2

#include <sys/ioctl.h>      // for ioctl
#include <sys/time.h>       // for gettimeofday
#include <unistd.h>         // for IO_NONBLOCK
#include <fcntl.h>          // for IO_NONBLOCK

#define VBI_EXT_OLD
//#define VBI_EXT_OLD_CMP
#ifdef VBI_EXT_OLD
#include "vbitext/cc.h"
#endif
#if !defined(VBI_EXT_OLD) || defined(VBI_EXT_OLD_CMP)
#include "vbi608extractor.h"
#endif
#include "mythcontext.h"
#include "mythlogging.h"
#include "v4lrecorder.h"
#include "vbitext/vbi.h"
#include "tv_rec.h"
#include "tv.h" // for VBIMode

#define TVREC_CARDNUM \
        ((tvrec != NULL) ? QString::number(tvrec->GetCaptureCardNum()) : "NULL")

#define LOC      QString("V4LRec(%1:%2): ") \
                 .arg(TVREC_CARDNUM).arg(videodevice)

V4LRecorder::V4LRecorder(TVRec *tv) :
    DTVRecorder(tv),          vbimode(VBIMode::None),
    pal_vbi_cb(NULL),         pal_vbi_tt(NULL),
    ntsc_vbi_width(0),        ntsc_vbi_start_line(0),
    ntsc_vbi_line_count(0),
    vbi608(NULL),
    vbi_thread(NULL),         vbi_fd(-1)
{
}

V4LRecorder::~V4LRecorder()
{
    if (vbi_thread)
    {
        delete vbi_thread;
        vbi_thread = NULL;
    }
}

void V4LRecorder::StopRecording(void)
{
    DTVRecorder::StopRecording();
    while (vbi_thread && vbi_thread->isRunning())
        vbi_thread->wait();
}

void V4LRecorder::SetOption(const QString &name, const QString &value)
{
    if (name == "audiodevice")
        audiodevice = value;
    else if (name == "vbidevice")
        vbidevice = value;
    else if (name == "vbiformat")
        vbimode = VBIMode::Parse(value);
    else
        DTVRecorder::SetOption(name, value);
}

static void vbi_event(struct VBIData *data, struct vt_event *ev)
{
    switch (ev->type)
    {
       case EV_PAGE:
       {
            struct vt_page *vtp = (struct vt_page *) ev->p1;
            if (vtp->flags & PG_SUBTITLE)
            {
#if 0
                LOG(VB_GENERAL, LOG_DEBUG, QString("subtitle page %1.%2")
                      .arg(vtp->pgno, 0, 16) .arg(vtp->subno, 0, 16));
#endif
                data->foundteletextpage = true;
                memcpy(&(data->teletextpage), vtp, sizeof(vt_page));
            }
       }

       case EV_HEADER:
       case EV_XPACKET:
           break;
    }
}

int V4LRecorder::OpenVBIDevice(void)
{
    int fd = -1;
    if (vbi_fd >= 0)
        return vbi_fd;

    struct VBIData *vbi_cb = NULL;
    struct vbi     *pal_tt = NULL;
    uint width = 0, start_line = 0, line_count = 0;
#if defined(VBI_EXT_OLD)
    uint rate = 0;
#endif

    QByteArray vbidev = vbidevice.toAscii();
    if (VBIMode::PAL_TT == vbimode)
    {
        pal_tt = vbi_open(vbidev.constData(), NULL, 99, -1);
        if (pal_tt)
        {
            fd = pal_tt->fd;
            vbi_cb = new VBIData;
            memset(vbi_cb, 0, sizeof(VBIData));
            vbi_cb->nvr = this;
            vbi_add_handler(pal_tt, (void*) vbi_event, vbi_cb);
        }
    }
    else if (VBIMode::NTSC_CC == vbimode)
    {
        fd = open(vbidev.constData(), O_RDONLY/*|O_NONBLOCK*/);
    }
    else
    {
        LOG(VB_GENERAL, LOG_ERR, LOC + "Invalid CC/Teletext mode");
        return -1;
    }

    if (fd < 0)
    {
        LOG(VB_GENERAL, LOG_ERR, LOC +
            QString("Can't open vbi device: '%1'").arg(vbidevice));
        return -1;
    }

    if (VBIMode::NTSC_CC == vbimode)
    {
#ifdef USING_V4L2
        struct v4l2_format fmt;
        memset(&fmt, 0, sizeof(fmt));
        fmt.type = V4L2_BUF_TYPE_VBI_CAPTURE;
        if (0 != ioctl(fd, VIDIOC_G_FMT, &fmt))
        {
#ifdef USING_V4L1
            LOG(VB_RECORD, LOG_INFO, "V4L2 VBI setup failed, trying v1 ioctl");
            // Try V4L v1 VBI ioctls, iff V4L v2 fails
            struct vbi_format old_fmt;
            memset(&old_fmt, 0, sizeof(vbi_format));
            if (ioctl(fd, VIDIOCGVBIFMT, &old_fmt) < 0)
            {
                LOG(VB_GENERAL, LOG_ERR, LOC +
                    "Failed to query vbi capabilities (V4L1)");
                close(fd);
                return -1;
            }
            fmt.fmt.vbi.sampling_rate    = old_fmt.sampling_rate;
            fmt.fmt.vbi.offset           = 0;
            fmt.fmt.vbi.samples_per_line = old_fmt.samples_per_line;
            fmt.fmt.vbi.start[0]         = old_fmt.start[0];
            fmt.fmt.vbi.start[1]         = old_fmt.start[1];
            fmt.fmt.vbi.count[0]         = old_fmt.count[0];
            fmt.fmt.vbi.count[1]         = old_fmt.count[1];
            fmt.fmt.vbi.flags            = old_fmt.flags;
#else // if !USING_V4L1
            LOG(VB_RECORD, LOG_ERR, "V4L2 VBI setup failed");
            close(fd);
            return -1;
#endif // !USING_V4L1
        }
        LOG(VB_RECORD, LOG_INFO, LOC +
            QString("vbi_format  rate: %1"
                    "\n\t\t\t          offset: %2"
                    "\n\t\t\tsamples_per_line: %3"
                    "\n\t\t\t          starts: %4, %5"
                    "\n\t\t\t          counts: %6, %7"
                    "\n\t\t\t           flags: 0x%8")
                .arg(fmt.fmt.vbi.sampling_rate)
                .arg(fmt.fmt.vbi.offset)
                .arg(fmt.fmt.vbi.samples_per_line)
                .arg(fmt.fmt.vbi.start[0])
                .arg(fmt.fmt.vbi.start[1])
                .arg(fmt.fmt.vbi.count[0])
                .arg(fmt.fmt.vbi.count[1])
                .arg(fmt.fmt.vbi.flags,0,16));

        width      = fmt.fmt.vbi.samples_per_line;
        start_line = fmt.fmt.vbi.start[0];
        line_count = fmt.fmt.vbi.count[0];
#if defined(VBI_EXT_OLD)
        rate       = fmt.fmt.vbi.sampling_rate;
#endif
        if (line_count != fmt.fmt.vbi.count[1])
        {
            LOG(VB_GENERAL, LOG_ERR, LOC +
                    "VBI must have the same number of "
                    "odd and even fields for our decoder");
            close(fd);
            return -1;
        }
        if (start_line > 21 || start_line + line_count < 22)
        {
            LOG(VB_GENERAL, LOG_ERR, LOC + "VBI does not include line 21");
            // TODO We could try to set the VBI format ourselves..
            close(fd);
            return -1;
        }
#endif // USING_V4L2
    }

    if (VBIMode::PAL_TT == vbimode)
    {
        pal_vbi_cb = vbi_cb;
        pal_vbi_tt = pal_tt;
    }
    else if (VBIMode::NTSC_CC == vbimode)
    {
        ntsc_vbi_width      = width;
        ntsc_vbi_start_line = start_line;
        ntsc_vbi_line_count = line_count;
#if defined(VBI_EXT_OLD)
        ntsc_vbi_rate       = rate;
#endif
#if !defined(VBI_EXT_OLD) || defined(VBI_EXT_OLD_CMP)
        vbi608 = new VBI608Extractor();
#endif
    }

    {
        QMutexLocker locker(&pauseLock);
        request_recording = true;
    }

    vbi_fd = fd;

    return fd;
}

void V4LRecorder::CloseVBIDevice(void)
{
    if (vbi_fd < 0)
        return;

    if (pal_vbi_tt)
    {
        vbi_del_handler(pal_vbi_tt, (void*) vbi_event, pal_vbi_cb);
        vbi_close(pal_vbi_tt);
        delete pal_vbi_cb;
        pal_vbi_cb = NULL;
    }
    else
    {
#if !defined(VBI_EXT_OLD) || defined(VBI_EXT_OLD_CMP)
        delete vbi608; vbi608 = NULL;
#endif
        close(vbi_fd);
    }

    vbi_fd = -1;
}

void V4LRecorder::RunVBIDevice(void)
{
    if (vbi_fd < 0)
        return;

    unsigned char *buf = NULL, *ptr = NULL, *ptr_end = NULL;
#if defined(VBI_EXT_OLD)
    struct cc *ntsc_cc = NULL;
    if (VBIMode::NTSC_CC == vbimode)
    {
        ntsc_cc = new struct cc;
        memset(ntsc_cc, 0, sizeof(struct cc));
        ntsc_cc->fd = vbi_fd;
        ntsc_cc->code1 = -1;
        ntsc_cc->code2 = -1;
        ntsc_cc->samples_per_line = ntsc_vbi_width;
        ntsc_cc->start_line       = ntsc_vbi_start_line;
        ntsc_cc->line_count       = ntsc_vbi_line_count;
        ntsc_cc->scale0           = (ntsc_vbi_rate + 503488 / 2) / 503488;
        ntsc_cc->scale1           = (ntsc_cc->scale0 * 2 + 3) / 5; /* 40% */
    }
#endif
    if (ntsc_vbi_width)
    {
        uint sz   = ntsc_vbi_width * ntsc_vbi_line_count * 2;
#if defined(VBI_EXT_OLD)
        buf = ptr = (unsigned char *) ntsc_cc->buffer;
#else
        buf = ptr = new unsigned char[sz];
#endif
        ptr_end   = buf + sz;
    }

    while (IsRecordingRequested() && !IsErrored())
    {
        if (PauseAndWait())
            continue;

        if (!IsRecordingRequested())
            break;

        struct timeval tv;
        fd_set rdset;

        tv.tv_sec = 0;
        tv.tv_usec = 5000;
        FD_ZERO(&rdset);
        FD_SET(vbi_fd, &rdset);

        int nr = select(vbi_fd + 1, &rdset, 0, 0, &tv);
        if (nr < 0)
            LOG(VB_GENERAL, LOG_ERR, LOC + "vbi select failed" + ENO);

        if (nr <= 0)
        {
#if 0
            if (nr==0)
                LOG(VB_GENERAL, LOG_ERR, LOC + "vbi select timed out");
#endif
            continue; // either failed or timed out..
        }
        if (VBIMode::PAL_TT == vbimode)
        {
            pal_vbi_cb->foundteletextpage = false;
            vbi_handler(pal_vbi_tt, pal_vbi_tt->fd);
            if (pal_vbi_cb->foundteletextpage)
            {
                // decode VBI as teletext subtitles
                FormatTT(pal_vbi_cb);
            }
        }
        else if (VBIMode::NTSC_CC == vbimode)
        {
            int ret = read(vbi_fd, ptr, ptr_end - ptr);
            ptr = (ret > 0) ? ptr + ret : ptr;
            if ((ptr_end - ptr) == 0)
            {
#if defined(VBI_EXT_OLD)
                cc_decode(ntsc_cc);
                FormatCC(ntsc_cc->code1, ntsc_cc->code2);
#endif
#if !defined(VBI_EXT_OLD) || defined(VBI_EXT_OLD_CMP)
                unsigned char *line21_field1 =
                    buf + ((21 - ntsc_vbi_start_line) * ntsc_vbi_width);
                unsigned char *line21_field2 =
                    buf + ((ntsc_vbi_line_count + 21 - ntsc_vbi_start_line)
                           * ntsc_vbi_width);
                bool cc1 = vbi608->ExtractCC12(line21_field1, ntsc_vbi_width);
                bool cc2 = vbi608->ExtractCC34(line21_field2, ntsc_vbi_width);
    #if defined(VBI_EXT_OLD_CMP)
                int code1 = vbi608->GetCode1();
                int code2 = vbi608->GetCode2();
                if ((code1 != ntsc_cc->code1) || (code2 != ntsc_cc->code2))
                    LOG(VB_VBI, LOG_INFO, QString("ext. old = %1/%2, new = (%3/%4) %5/%6")
                                           .arg(ntsc_cc->code1, 4, 16).arg(ntsc_cc->code2, 4, 16)
                                           .arg(cc1).arg(cc2)
                                           .arg(code1, 4, 16).arg(code2, 4, 16));
    #else
                if (cc1 || cc2)
                {
                    int code1 = vbi608->GetCode1();
                    int code2 = vbi608->GetCode2();
                    code1 = (0xFFFF==code1) ? -1 : code1;
                    code2 = (0xFFFF==code2) ? -1 : code2;
                    FormatCC(code1, code2);
                }
    #endif
#endif
                ptr = buf;
            }
            else if (ret < 0)
            {
                LOG(VB_GENERAL, LOG_ERR, LOC + "Reading VBI data" + ENO);
            }
        }
    }

#if defined(VBI_EXT_OLD)
    if (ntsc_cc)
        delete ntsc_cc;
#else
    if (buf)
        delete [] buf;
#endif

    CloseVBIDevice();
}

/* vim: set expandtab tabstop=4 shiftwidth=4: */
