/*
 * Copyright (c) 2004 Doug Larrick <doug@ties.org>.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include <cstdio>
#include <cerrno>
#include <cmath>
#include <cstdlib> // for abs(int)
#include <unistd.h>
#include <fcntl.h>

#include <sys/types.h>
#include <sys/stat.h>
#include "compat.h"

#ifndef _WIN32
#include <sys/ioctl.h>
#include <sys/poll.h>
#endif

#include "mythcontext.h"
#include "mythmainwindow.h"

#ifdef USING_XV
#include "videoout_xv.h"
#endif

#ifdef USING_VDPAU
#include "videoout_vdpau.h"
#endif

#ifdef USING_OPENGL_VSYNC
#include "mythrender_opengl.h"
#endif // USING_OPENGL_VSYNC

#ifdef __linux__
#include <linux/rtc.h>
#endif

using namespace std;
#include "videooutbase.h"
#include "vsync.h"

bool tryingVideoSync = false;
int VideoSync::m_forceskip = 0;

#define TESTVIDEOSYNC(NAME) \
    do { if (++m_forceskip > skip) \
    { \
        trial = new NAME (video_output,     frame_interval, \
                          refresh_interval, halve_frame_interval); \
        if (trial->TryInit()) \
        { \
            m_forceskip = skip; \
            tryingVideoSync = false; \
            return trial; \
        } \
        delete trial; \
    } } while (false)

#define LOC QString("VSYNC: ")

/** \fn VideoSync::BestMethod(VideoOutput*,uint,uint,bool)
 *  \brief Returns the most sophisticated video sync method available.
 */
VideoSync *VideoSync::BestMethod(VideoOutput *video_output,
                                 uint frame_interval, uint refresh_interval,
                                 bool halve_frame_interval)
{
    VideoSync *trial = NULL;
    tryingVideoSync  = true;

    // m_forceskip allows for skipping one sync method
    // due to crash on the previous run.
    int skip = 0;
    if (m_forceskip)
    {
        VERBOSE(VB_PLAYBACK, LOC +
            QString("A previous trial crashed, skipping %1").arg(m_forceskip));

        skip = m_forceskip;
        m_forceskip = 0;
    }

#ifdef USING_VDPAU
//    TESTVIDEOSYNC(VDPAUVideoSync);
#endif
#ifndef _WIN32
    TESTVIDEOSYNC(DRMVideoSync);
#ifdef USING_OPENGL_VSYNC
/*
    if (gCoreContext->GetNumSetting("UseOpenGLVSync", 1) &&
       (getenv("NO_OPENGL_VSYNC") == NULL))
    {
        TESTVIDEOSYNC(OpenGLVideoSync);
    }
*/
#endif
#endif // _WIN32
#ifdef __linux__
    TESTVIDEOSYNC(RTCVideoSync);
#endif // __linux__

    TESTVIDEOSYNC(BusyWaitVideoSync);

    tryingVideoSync=false;
    return NULL;
}

/** \fn VideoSync::VideoSync(VideoOutput*,int,int,bool)
 *  \brief Used by BestMethod(VideoOutput*,uint,uint,bool) to initialize
 *         video synchronization method.
 */
VideoSync::VideoSync(VideoOutput *video_output,
                     int frameint, int refreshint,
                     bool halve_frame_interval) :
    m_video_output(video_output),   m_frame_interval(frameint),
    m_refresh_interval(refreshint), m_interlaced(halve_frame_interval),
    m_nexttrigger(0), m_delay(-1)
{
}

int64_t VideoSync::GetTime(void)
{
    struct timeval now_tv;
    gettimeofday(&now_tv, NULL);
    return now_tv.tv_sec * 1000000LL + now_tv.tv_usec;
}

void VideoSync::Start(void)
{
    m_nexttrigger = GetTime();
}

/** \fn VideoSync::CalcDelay()
 *  \brief Calculates the delay to the next frame.
 *
 *   Regardless of the timing method, if delay is greater than four full
 *   frames (could be greater than 20 or greater than 200), we don't want
 *   to freeze while waiting for a huge delay. Instead, contine playing
 *   video at half speed and continue to read new audio and video frames
 *   from the file until the sync is 'in the ballpark'.
 *   Also prevent the nexttrigger from falling too far in the past in case
 *   we are trying to speed up video output faster than possible.
 */
int VideoSync::CalcDelay()
{
    int64_t now = GetTime();
    //cout << "CalcDelay: next: " << timeval_str(m_nexttrigger) << " now "
    // << timeval_str(now) << endl;

    int ret_val = m_nexttrigger - now;

    //cout << "delay " << ret_val << endl;

    if (ret_val > m_frame_interval * 4)
    {
        if (m_interlaced)
            ret_val = (m_frame_interval / 2) * 4;
        else
            ret_val = m_frame_interval * 4;

        // set nexttrigger to our new target time
        m_nexttrigger = now + ret_val;
    }

    if (ret_val < -m_frame_interval)
    {
        ret_val = -m_frame_interval;

        // set nexttrigger to our new target time
        m_nexttrigger = now + ret_val;
    }

    return ret_val;
}

/** \fn VideoSync::KeepPhase()
 *  \brief Keep our nexttrigger from drifting too close to the exact retrace.
 *
 *   If delay is near zero, some frames will be delay < 0 and others
 *   delay > 0 which would cause continous rapid fire stuttering.
 *   This method is only useful for those sync methods where WaitForFrame
 *   targets hardware retrace rather than targeting nexttrigger.
 */
void VideoSync::KeepPhase()
{
    // cerr << m_delay << endl;
    if (m_delay < -(m_refresh_interval/2))
        m_nexttrigger += 200;
    else if (m_delay > -500)
        m_nexttrigger += -2000;
}

#ifndef _WIN32
#define DRM_VBLANK_RELATIVE 0x1;

struct drm_wait_vblank_request {
    int type;
    unsigned int sequence;
    unsigned long signal;
};

struct drm_wait_vblank_reply {
    int type;
    unsigned int sequence;
    long tval_sec;
    long tval_usec;
};

typedef union drm_wait_vblank {
    struct drm_wait_vblank_request request;
    struct drm_wait_vblank_reply reply;
} drm_wait_vblank_t;

#define DRM_IOCTL_BASE                  'd'
#define DRM_IOWR(nr,type)               _IOWR(DRM_IOCTL_BASE,nr,type)

#define DRM_IOCTL_WAIT_VBLANK           DRM_IOWR(0x3a, drm_wait_vblank_t)

static int drmWaitVBlank(int fd, drm_wait_vblank_t *vbl)
{
    int ret = -1;

    do {
       ret = ioctl(fd, DRM_IOCTL_WAIT_VBLANK, vbl);
       vbl->request.type &= ~DRM_VBLANK_RELATIVE;
    } while (ret && errno == EINTR);

    return ret;
}

const char *DRMVideoSync::sm_dri_dev = "/dev/dri/card0";

DRMVideoSync::DRMVideoSync(VideoOutput *vo, int fr, int ri, bool intl) :
    VideoSync(vo, fr, ri, intl)
{
    m_dri_fd = -1;
}

DRMVideoSync::~DRMVideoSync()
{
    if (m_dri_fd >= 0)
        close(m_dri_fd);
    m_dri_fd = -1;
}

bool DRMVideoSync::TryInit(void)
{
    drm_wait_vblank_t blank;

    m_dri_fd = open(sm_dri_dev, O_RDWR);
    if (m_dri_fd < 0)
    {
        VERBOSE(VB_PLAYBACK, LOC + QString("DRMVideoSync: Could not open device"
                " %1, %2").arg(sm_dri_dev).arg(strerror(errno)));
        return false; // couldn't open device
    }

    blank.request.type = DRM_VBLANK_RELATIVE;
    blank.request.sequence = 1;
    if (drmWaitVBlank(m_dri_fd, &blank))
    {
        VERBOSE(VB_PLAYBACK, LOC + QString("DRMVideoSync: VBlank ioctl did not"
                " work, unimplemented in this driver?"));
        return false; // VBLANK ioctl didn't worko
    }

    return true;
}

void DRMVideoSync::Start(void)
{
    // Wait for a refresh so we start out synched
    drm_wait_vblank_t blank;
    blank.request.type = DRM_VBLANK_RELATIVE;
    blank.request.sequence = 1;
    drmWaitVBlank(m_dri_fd, &blank);
    VideoSync::Start();
}

void DRMVideoSync::WaitForFrame(int sync_delay)
{
    // Offset for externally-provided A/V sync delay
    m_nexttrigger += sync_delay;

    m_delay = CalcDelay();
    //cerr << "WaitForFrame at : " << m_delay;

    // Always sync to the next retrace execpt when we are very late.
    if (m_delay > -(m_refresh_interval/2))
    {
        drm_wait_vblank_t blank;
        blank.request.type = DRM_VBLANK_RELATIVE;
        blank.request.sequence = 1;
        drmWaitVBlank(m_dri_fd, &blank);
        m_delay = CalcDelay();
        // cerr << "\tDelay at sync: " << m_delay;
    }
    //cerr  << endl;

    if (m_delay > 0)
    {
        // Wait for any remaining retrace intervals in one pass.
        int n = m_delay / m_refresh_interval + 1;

        drm_wait_vblank_t blank;
        blank.request.type = DRM_VBLANK_RELATIVE;
        blank.request.sequence = n;
        drmWaitVBlank(m_dri_fd, &blank);
        m_delay = CalcDelay();
        //cerr << "Wait " << n << " intervals. Count " << blank.request.sequence;
        //cerr  << " Delay " << m_delay << endl;
    }

    KeepPhase();
}
#endif /* !_WIN32 */

#ifndef _WIN32
OpenGLVideoSync::OpenGLVideoSync(VideoOutput *video_output,
                                 int frame_interval, int refresh_interval,
                                 bool interlaced) :
    VideoSync(video_output, frame_interval, refresh_interval, interlaced),
    m_context(NULL), m_device(NULL)
{
    VERBOSE(VB_IMPORTANT, LOC + "OpenGLVideoSync()");
}

OpenGLVideoSync::~OpenGLVideoSync()
{
    VERBOSE(VB_IMPORTANT, LOC + "~OpenGLVideoSync()");
#ifdef USING_OPENGL_VSYNC
    if (m_context)
        delete m_context;
    if (m_device)
        delete m_device;
#endif
}

/** \fn OpenGLVideoSync::TryInit(void)
 *  \brief Try to create an OpenGL surface so we can use glXWaitVideoSyncSGI:
 *  \return true if this method can be employed, false if it cannot.
 */
bool OpenGLVideoSync::TryInit(void)
{
#ifdef USING_OPENGL_VSYNC
    m_device = new QPixmap(16,16);
    QGLFormat fmt;
    fmt.setDepth(false);
    m_context = new MythRenderOpenGL(fmt, m_device);
    if (m_context && m_context->create())
    {
        m_context->Init();
        if (m_context->HasGLXWaitVideoSyncSGI())
            return true;

        VERBOSE(VB_IMPORTANT, LOC +
            "OpenGLVideoSync: GLX_SGI_video_sync extension not "
            "supported by driver.");
    }

    VERBOSE(VB_PLAYBACK, LOC + "OpenGLVideoSync: "
            "Failed to Initialize OpenGL V-Sync");

#endif /* USING_OPENGL_VSYNC */
    return false;
}

void OpenGLVideoSync::Start(void)
{
#ifdef USING_OPENGL_VSYNC
    if (!m_context)
        return;

    if (!(m_video_output && m_video_output->IsEmbedding()))
    {
        unsigned int count = m_context->GetVideoSyncCount();
        m_context->WaitForVideoSync(2, (count+1)%2, &count);
    }
    // Initialize next trigger
    VideoSync::Start();
#endif /* USING_OPENGL_VSYNC */
}

void OpenGLVideoSync::WaitForFrame(int sync_delay)
{
    (void) sync_delay;
#ifdef USING_OPENGL_VSYNC
    const QString msg1("First A/V Sync"), msg2("Second A/V Sync");
    m_nexttrigger += sync_delay;

    if (m_video_output && m_video_output->IsEmbedding())
    {
        m_delay = CalcDelay();
        if (m_delay > 0)
            usleep(m_delay);
        return;
    }

    if (!m_context)
        return;

    unsigned int frameNum = m_context->GetVideoSyncCount();

    // Always sync to the next retrace execpt when we are very late.
    if ((m_delay = CalcDelay()) > -(m_refresh_interval/2))
    {
        m_context->WaitForVideoSync(2, (frameNum+1)%2 ,&frameNum);
        m_delay = CalcDelay();
    }

    // Wait for any remaining retrace intervals in one pass.
    if (m_delay > 0)
    {
        uint n = m_delay / m_refresh_interval + 1;
        m_context->WaitForVideoSync((n+1), (frameNum+n)%(n+1), &frameNum);
        m_delay = CalcDelay();
    }

    KeepPhase();
#endif /* USING_OPENGL_VSYNC */
}
#endif /* !_WIN32 */

#ifdef __linux__
#define RTCRATE 1024
RTCVideoSync::RTCVideoSync(VideoOutput *vo, int fi, int ri, bool intr) :
    VideoSync(vo, fi, ri, intr)
{
    m_rtcfd = -1;
}

RTCVideoSync::~RTCVideoSync()
{
    if (m_rtcfd >= 0)
        close(m_rtcfd);
}

bool RTCVideoSync::TryInit(void)
{
    m_rtcfd = open("/dev/rtc", O_RDONLY);
    if (m_rtcfd < 0)
    {
        VERBOSE(VB_PLAYBACK, LOC + QString("RTCVideoSync: Could not"
                " open /dev/rtc, %1.").arg(strerror(errno)));
        return false;
    }

    // FIXME, does it make sense to tie RTCRATE to the desired framerate?
    if ((ioctl(m_rtcfd, RTC_IRQP_SET, RTCRATE) < 0))
    {
        VERBOSE(VB_PLAYBACK, LOC + QString("RTCVideoSync: Could not"
                " set RTC frequency, %1.").arg(strerror(errno)));
        return false;
    }

    if (ioctl(m_rtcfd, RTC_PIE_ON, 0) < 0)
    {
        VERBOSE(VB_PLAYBACK, LOC + QString("RTCVideoSync: Could not enable "
                "periodic timer interrupts, %1.").arg(strerror(errno)));
        return false;
    }

    return true;
}

void RTCVideoSync::WaitForFrame(int sync_delay)
{
    m_nexttrigger += sync_delay;

    m_delay = CalcDelay();

    unsigned long rtcdata;
    while (m_delay > 0)
    {
        ssize_t val = read(m_rtcfd, &rtcdata, sizeof(rtcdata));
        m_delay = CalcDelay();

        if ((val < 0) && (m_delay > 0))
            usleep(m_delay);
    }
}
#endif /* __linux__ */

#ifdef USING_VDPAU
VDPAUVideoSync::VDPAUVideoSync(VideoOutput *vo,
                              int fr, int ri, bool intl) :
    VideoSync(vo, fr, ri, intl)
{
}

VDPAUVideoSync::~VDPAUVideoSync()
{
}

bool VDPAUVideoSync::TryInit(void)
{
    VideoOutputVDPAU *vo = dynamic_cast<VideoOutputVDPAU*>(m_video_output);
    if (!vo)
        return false;

    return true;
}

void VDPAUVideoSync::WaitForFrame(int sync_delay)
{
    // Offset for externally-provided A/V sync delay
    m_nexttrigger += sync_delay;
    m_delay = CalcDelay();

    if (m_delay < 0)
        m_delay = 0;

    VideoOutputVDPAU *vo = (VideoOutputVDPAU *)(m_video_output);
    vo->SetNextFrameDisplayTimeOffset(m_delay);
}
#endif

BusyWaitVideoSync::BusyWaitVideoSync(VideoOutput *vo,
                                     int fr, int ri, bool intl) :
    VideoSync(vo, fr, ri, intl)
{
    m_cheat = 5000;
    m_fudge = 0;
}

BusyWaitVideoSync::~BusyWaitVideoSync()
{
}

bool BusyWaitVideoSync::TryInit(void)
{
    return true;
}

void BusyWaitVideoSync::WaitForFrame(int sync_delay)
{
    // Offset for externally-provided A/V sync delay
    m_nexttrigger += sync_delay;

    m_delay = CalcDelay();

    if (m_delay > 0)
    {
        int cnt = 0;
        m_cheat += 100;
        // The usleep() is shortened by "cheat" so that this process gets
        // the CPU early for about half the frames.
        if (m_delay > (m_cheat - m_fudge))
            usleep(m_delay - (m_cheat - m_fudge));

        // If late, draw the frame ASAP.  If early, hold the CPU until
        // half as late as the previous frame (fudge).
        m_delay = CalcDelay();
        m_fudge = min(m_fudge, m_frame_interval);
        while (m_delay + m_fudge > 0)
        {
            m_delay = CalcDelay();
            cnt++;
        }
        m_fudge = abs(m_delay / 2);
        if (cnt > 1)
            m_cheat -= 200;
    }
}

USleepVideoSync::USleepVideoSync(VideoOutput *vo,
                                 int fr, int ri, bool intl) :
    VideoSync(vo, fr, ri, intl)
{
}

USleepVideoSync::~USleepVideoSync()
{
}

bool USleepVideoSync::TryInit(void)
{
    return true;
}

void USleepVideoSync::WaitForFrame(int sync_delay)
{
    // Offset for externally-provided A/V sync delay
    m_nexttrigger += sync_delay;

    m_delay = CalcDelay();
    if (m_delay > 0)
        usleep(m_delay);
}

