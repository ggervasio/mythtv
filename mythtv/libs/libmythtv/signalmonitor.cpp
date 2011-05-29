// -*- Mode: c++ -*-
// Copyright (c) 2005, Daniel Thor Kristjansson

// C headers
#include <sys/types.h>
#include <signal.h>
#include <unistd.h>

// MythTV headers
#include "scriptsignalmonitor.h"
#include "signalmonitor.h"
#include "mythcontext.h"
#include "compat.h"
#include "tv_rec.h"

extern "C" {
#include "libavcodec/avcodec.h"
}
#include "util.h"

#ifdef USING_DVB
#   include "dvbsignalmonitor.h"
#   include "dvbchannel.h"
#endif

#ifdef USING_V4L2
#   include "analogsignalmonitor.h"
#   include "v4lchannel.h"
#endif

#ifdef USING_HDHOMERUN
#   include "hdhrsignalmonitor.h"
#   include "hdhrchannel.h"
#endif

#ifdef USING_IPTV
#   include "iptvsignalmonitor.h"
#   include "iptvchannel.h"
#endif

#ifdef USING_FIREWIRE
#   include "firewiresignalmonitor.h"
#   include "firewirechannel.h"
#endif

#ifdef USING_ASI
#   include "asisignalmonitor.h"
#   include "asichannel.h"
#endif

#undef DBG_SM
#define DBG_SM(FUNC, MSG) VERBOSE(VB_CHANNEL, \
    "SM("<<channel->GetDevice()<<")::"<<FUNC<<": "<<MSG);

/** \class SignalMonitor
 *  \brief Signal monitoring base class.
 *
 *   Signal monitors should extend this to add signal monitoring to their
 *   recorder. All signal monitors must implement one signals, the
 *   StatusSignalLock(int) signal. The lock signal should only be set to
 *   true when it is absolutely safe to begin or to continue recording.
 *   The optional StatusSignalStrength signal should report the actual
 *   signal value.
 *
 *   Additional signals may be implemented, see DTVSignalMonitor and
 *   DVBSignalMonitor for example.
 *
 *  \sa AnalocSignalMonitor, DTVSignalMonitor, DVBSignalMonitor,
        HDHRSignalMonitor, SignalMonitorValue
 */

static void ALRMhandler(int /*sig*/)
{
     cerr<<"SignalMonitor: Got SIGALRM"<<endl;
     signal(SIGINT, ALRMhandler);
}

SignalMonitor *SignalMonitor::Init(QString cardtype, int db_cardnum,
                                   ChannelBase *channel)
{
    (void) cardtype;
    (void) db_cardnum;
    (void) channel;

    SignalMonitor *signalMonitor = NULL;

    {
        QMutexLocker locker(avcodeclock);
        avcodec_init();
    }

#ifdef USING_DVB
    if (CardUtil::IsDVBCardType(cardtype))
    {
        DVBChannel *dvbc = dynamic_cast<DVBChannel*>(channel);
        if (dvbc)
            signalMonitor = new DVBSignalMonitor(db_cardnum, dvbc);
    }
#endif

#ifdef USING_V4L2
    if ((cardtype.toUpper() == "HDPVR"))
    {
        V4LChannel *chan = dynamic_cast<V4LChannel*>(channel);
        if (chan)
            signalMonitor = new AnalogSignalMonitor(db_cardnum, chan);
    }
#endif

#ifdef USING_HDHOMERUN
    if (cardtype.toUpper() == "HDHOMERUN")
    {
        HDHRChannel *hdhrc = dynamic_cast<HDHRChannel*>(channel);
        if (hdhrc)
            signalMonitor = new HDHRSignalMonitor(db_cardnum, hdhrc);
    }
#endif

#ifdef USING_IPTV
    if (cardtype.toUpper() == "FREEBOX")
    {
        IPTVChannel *fbc = dynamic_cast<IPTVChannel*>(channel);
        if (fbc)
            signalMonitor = new IPTVSignalMonitor(db_cardnum, fbc);
    }
#endif

#ifdef USING_FIREWIRE
    if (cardtype.toUpper() == "FIREWIRE")
    {
        FirewireChannel *fc = dynamic_cast<FirewireChannel*>(channel);
        if (fc)
            signalMonitor = new FirewireSignalMonitor(db_cardnum, fc);
    }
#endif

#ifdef USING_ASI
    if (cardtype.toUpper() == "ASI")
    {
        ASIChannel *fc = dynamic_cast<ASIChannel*>(channel);
        if (fc)
            signalMonitor = new ASISignalMonitor(db_cardnum, fc);
    }
#endif

    if (!signalMonitor && channel)
    {
        signalMonitor = new ScriptSignalMonitor(db_cardnum, channel);
    }

    if (!signalMonitor)
    {
        VERBOSE(VB_IMPORTANT,
                QString("Failed to create signal monitor in Init(%1, %2, 0x%3)")
                .arg(cardtype).arg(db_cardnum).arg((long)channel,0,16));
    }

    return signalMonitor;
}

/**
 *  \brief Initializes signal lock and signal values.
 *
 *   Start() must be called to actually begin continuous
 *   signal monitoring.
 *
 *  \param db_cardnum Recorder number to monitor,
 *         if this is less than 0, SIGNAL events will not be
 *         sent to the frontend even if SetNotifyFrontend(true)
 *         is called.
 *  \param _channel      ChannelBase class for our monitoring
 *  \param wait_for_mask SignalMonitorFlags to start with.
 */
SignalMonitor::SignalMonitor(int _capturecardnum, ChannelBase *_channel,
                             uint64_t wait_for_mask)
    : channel(_channel),
      capturecardnum(_capturecardnum), flags(wait_for_mask),
      update_rate(25),                 minimum_update_rate(5),
      running(false),                  exit(false),
      update_done(false),              notify_frontend(true),
      eit_scan(false),
      signalLock    (QObject::tr("Signal Lock"),  "slock",
                     1, true, 0,   1, 0),
      signalStrength(QObject::tr("Signal Power"), "signal",
                     0, true, 0, 100, 0),
      scriptStatus  (QObject::tr("Script Status"), "script",
                     3, true, 0, 3, 0),
      statusLock(QMutex::Recursive)
{
    if (!channel->IsExternalChannelChangeSupported())
    {
        scriptStatus.SetValue(3);
    }
}

/** \fn SignalMonitor::~SignalMonitor()
 *  \brief Stops monitoring thread.
 */
SignalMonitor::~SignalMonitor()
{
    Stop();
}

void SignalMonitor::AddFlags(uint64_t _flags)
{
    DBG_SM("AddFlags", sm_flags_to_string(_flags));
    flags |= _flags;
}

void SignalMonitor::RemoveFlags(uint64_t _flags)
{
    DBG_SM("RemoveFlags", sm_flags_to_string(_flags));
    flags &= ~_flags;
}

bool SignalMonitor::HasFlags(uint64_t _flags) const
{
    return (flags & _flags) == _flags;
}

bool SignalMonitor::HasAnyFlag(uint64_t _flags) const
{
    return (flags & _flags);
}

/** \fn SignalMonitor::Start()
 *  \brief Start signal monitoring thread.
 */
void SignalMonitor::Start()
{
    DBG_SM("Start", "begin");
    {
        QMutexLocker locker(&startStopLock);

        start();

        while (!running)
            usleep(5000);
    }
    DBG_SM("Start", "end");
}

/** \fn SignalMonitor::Stop()
 *  \brief Stop signal monitoring thread.
 */
void SignalMonitor::Stop()
{
    DBG_SM("Stop", "begin");
    {
        QMutexLocker locker(&startStopLock);
        if (running)
        {
            exit = true;
            wait();
        }
    }
    DBG_SM("Stop", "end");
}

/** \fn SignalMonitor::Kick()
 *  \brief Wake up monitor thread, and wait for
 *         UpdateValue() to execute once.
 */
void SignalMonitor::Kick()
{
    update_done = false;

    //pthread_kill(monitor_thread, SIGALRM);

    while (!update_done)
        usleep(50);
}

/** \fn SignalMonitor::GetStatusList(bool)
 *  \brief Returns QStringList containing all signals and their current
 *         values.
 *
 *   This serializes the signal monitoring values so that they can
 *   be passed from a backend to a frontend.
 *
 *   SignalMonitorValue::Parse(const QStringList&) will convert this
 *   to a vector of SignalMonitorValue instances.
 *
 *  \param kick if true Kick() will be employed so that this
 *         call will not have to wait for the next signal
 *         monitoring event.
 */
QStringList SignalMonitor::GetStatusList(bool kick)
{
    if (kick && running)
        Kick();
    else if (!running)
        UpdateValues();

    QStringList list;
    statusLock.lock();
    list<<scriptStatus.GetName()<<scriptStatus.GetStatus();
    list<<signalLock.GetName()<<signalLock.GetStatus();
    if (HasFlags(kSigMon_WaitForSig))
        list<<signalStrength.GetName()<<signalStrength.GetStatus();
    statusLock.unlock();

    return list;
}

/** \fn SignalMonitor::MonitorLoop()
 *  \brief Basic signal monitoring loop
 */
void SignalMonitor::MonitorLoop()
{
    running = true;
    exit = false;

    while (!exit)
    {
        UpdateValues();

        if (notify_frontend && capturecardnum>=0)
        {
            QStringList slist = GetStatusList(false);
            MythEvent me(QString("SIGNAL %1").arg(capturecardnum), slist);
            gCoreContext->dispatch(me);
            //cerr<<"sent SIGNAL"<<endl;
        }

        usleep(update_rate * 1000);
    }

    // We need to send a last informational message because a
    // signal update may have come in while we were sleeping
    // if we are using the multithreaded dtvsignalmonitor.
    if (notify_frontend && capturecardnum>=0)
    {
        QStringList slist = GetStatusList(false);
        MythEvent me(QString("SIGNAL %1").arg(capturecardnum), slist);
        gCoreContext->dispatch(me);
    }
    running = false;
}

/** \fn  SignalMonitor::WaitForLock(int)
 *  \brief Wait for a StatusSignaLock(int) of true.
 *
 *   This can be called whether or not the signal
 *   monitoring thread has been started.
 *
 *  \param timeout maximum time to wait in milliseconds.
 *  \return true if signal was acquired.
 */
bool SignalMonitor::WaitForLock(int timeout)
{
    statusLock.lock();
    if (-1 == timeout)
        timeout = signalLock.GetTimeout();
    statusLock.unlock();
    if (timeout<0)
        return false;

    MythTimer t;
    t.start();
    if (running)
    {
        while (t.elapsed()<timeout && running)
        {
            Kick();

            if (HasSignalLock())
                return true;

            usleep(50);
        }
        if (!running)
            return WaitForLock(timeout-t.elapsed());
    }
    else
    {
        while (t.elapsed()<timeout && !running)
        {
            UpdateValues();

            if (HasSignalLock())
                return true;

            usleep(50);
        }
        if (running)
            return WaitForLock(timeout-t.elapsed());
    }
    return false;
}

void SignalMonitor::AddListener(SignalMonitorListener *listener)
{
    QMutexLocker locker(&listenerLock);
    for (uint i = 0; i < listeners.size(); i++)
    {
        if (listeners[i] == listener)
            return;
    }
    listeners.push_back(listener);
}

void SignalMonitor::RemoveListener(SignalMonitorListener *listener)
{
    QMutexLocker locker(&listenerLock);

    vector<SignalMonitorListener*> new_listeners;
    for (uint i = 0; i < listeners.size(); i++)
    {
        if (listeners[i] != listener)
            new_listeners.push_back(listeners[i]);
    }

    listeners = new_listeners;
}

void SignalMonitor::SendMessage(
    SignalMonitorMessageType type, const SignalMonitorValue &value)
{
    statusLock.lock();
    SignalMonitorValue val = value;
    statusLock.unlock();

    QMutexLocker locker(&listenerLock);
    for (uint i = 0; i < listeners.size(); i++)
    {
        SignalMonitorListener *listener = listeners[i];
        DVBSignalMonitorListener *dvblistener =
            dynamic_cast<DVBSignalMonitorListener*>(listener);

        switch (type)
        {
        case kStatusSignalLock:
            listener->StatusSignalLock(val);
            break;
        case kAllGood:
            listener->AllGood();
            break;
        case kStatusSignalStrength:
            listener->StatusSignalStrength(val);
            break;
        case kStatusChannelTuned:
            listener->StatusChannelTuned(val);
            break;
        case kStatusSignalToNoise:
            if (dvblistener)
                dvblistener->StatusSignalToNoise(val);
            break;
        case kStatusBitErrorRate:
            if (dvblistener)
                dvblistener->StatusBitErrorRate(val);
            break;
        case kStatusUncorrectedBlocks:
            if (dvblistener)
                dvblistener->StatusUncorrectedBlocks(val);
            break;
        case kStatusRotorPosition:
            if (dvblistener)
                dvblistener->StatusRotorPosition(val);
            break;
        }
    }
}

void SignalMonitor::UpdateValues(void)
{
    QMutexLocker locker(&statusLock);
    if (channel->IsExternalChannelChangeSupported() &&
        (scriptStatus.GetValue() < 2))
    {
        scriptStatus.SetValue(channel->GetScriptStatus());
    }
}

void SignalMonitor::SendMessageAllGood(void)
{
    QMutexLocker locker(&listenerLock);
    for (uint i = 0; i < listeners.size(); i++)
        listeners[i]->AllGood();
}

void SignalMonitor::EmitStatus(void)
{
    SendMessage(kStatusChannelTuned, scriptStatus);
    SendMessage(kStatusSignalLock, signalLock);
    if (HasFlags(kSigMon_WaitForSig))
        SendMessage(kStatusSignalStrength,    signalStrength);
}
