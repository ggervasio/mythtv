/** -*- Mode: c++ -*-
 *  IPTVFeederFile
 *
 *  Please, don't submit bug reports if it
 *  can't read your file. Just use MythVideo!
 *
 *  Copyright (c) 2006 by Mike Mironov & Mickaël Remars
 *  Distributed as part of MythTV under GPL v2 and later.
 */
#include <algorithm>

#include "iptvfeederfile.h"

// Qt headers
#include <QUrl>

// Live555 headers
#include <BasicUsageEnvironment.hh>
#include <Groupsock.hh>
#include <GroupsockHelper.hh>
#include <ByteStreamFileSource.hh>
#include <TunnelEncaps.hh>

// MythTV headers
#include "iptvmediasink.h"
#include "mythcontext.h"
#include "mythlogging.h"
#include "tspacket.h"

#define LOC QString("FbFeedFile: ")
#define LOC_ERR QString("FbFeedFile, Error: ")


IPTVFeederFile::IPTVFeederFile() :
    _source(NULL),
    _sink(NULL)
{
}

IPTVFeederFile::~IPTVFeederFile()
{
    Close();
}

bool IPTVFeederFile::IsFile(const QString &url)
{
    return url.startsWith("file:", Qt::CaseInsensitive);
}

bool IPTVFeederFile::Open(const QString &url)
{
    VERBOSE(VB_RECORD, LOC + QString("Open(%1) -- begin").arg(url));

    QMutexLocker locker(&_lock);

    if (_source)
    {
        VERBOSE(VB_RECORD, LOC + "Open() -- end 1");
        return true;
    }

    QUrl parse(url);
    if (parse.path().isEmpty() || (parse.scheme().toLower() != "file"))
    {
        VERBOSE(VB_IMPORTANT, LOC_ERR + "Open() -- end 2");
        return false;
    }

    // Begin by setting up our usage environment:
    if (!InitEnv())
        return false;

    QString pathstr = parse.toLocalFile();
    QByteArray path = pathstr.toLocal8Bit();

    _source = ByteStreamFileSource::createNew(
        *_live_env, path.constData());
    if (!_source)
    {
        VERBOSE(VB_IMPORTANT, LOC + "Failed to create Live File Source.");
        FreeEnv();
        return false;
    }

    _sink = IPTVMediaSink::CreateNew(*_live_env, TSPacket::kSize * 128*1024);
    if (!_sink)
    {
        VERBOSE(VB_IMPORTANT,
                QString("IPTV # Failed to create sink: %1")
                .arg(_live_env->getResultMsg()));

        Medium::close(_source);
        _source = NULL;
        FreeEnv();

        return false;
    }

    _sink->startPlaying(*_source, NULL, NULL);
    vector<TSDataListener*>::iterator it = _listeners.begin();
    for (; it != _listeners.end(); ++it)
        _sink->AddListener(*it);

    VERBOSE(VB_RECORD, LOC + "Open() -- end");

    return true;
}

void IPTVFeederFile::Close(void)
{
    VERBOSE(VB_RECORD, LOC + "Close() -- begin");
    Stop();

    QMutexLocker locker(&_lock);

    if (_sink)
    {
        Medium::close(_sink);
        _sink = NULL;
    }

    if (_source)
    {
        Medium::close(_source);
        _source = NULL;
    }

    FreeEnv();

    VERBOSE(VB_RECORD, LOC + "Close() -- end");
}

void IPTVFeederFile::AddListener(TSDataListener *item)
{
    VERBOSE(VB_RECORD, LOC + QString("AddListener(0x%1) -- begin")
                       .arg((uint64_t)item,0,16));
    if (!item)
    {
        VERBOSE(VB_RECORD, LOC + QString("AddListener(0x%1) -- end")
                           .arg((uint64_t)item,0,16));
        return;
    }

    // avoid duplicates
    RemoveListener(item);

    // add to local list
    QMutexLocker locker(&_lock);
    _listeners.push_back(item);

    if (_sink)
        _sink->AddListener(item);

    VERBOSE(VB_RECORD, LOC + QString("AddListener(0x%1) -- end")
                       .arg((uint64_t)item,0,16));
}

void IPTVFeederFile::RemoveListener(TSDataListener *item)
{
    VERBOSE(VB_RECORD, LOC + QString("RemoveListener(0x%1) -- begin")
                       .arg((uint64_t)item,0,16));
    QMutexLocker locker(&_lock);
    vector<TSDataListener*>::iterator it =
        find(_listeners.begin(), _listeners.end(), item);

    if (it == _listeners.end())
    {
        VERBOSE(VB_RECORD, LOC + QString("RemoveListener(0x%1) -- end 1")
                           .arg((uint64_t)item,0,16));
        return;
    }

    // remove from local list..
    *it = *_listeners.rbegin();
    _listeners.resize(_listeners.size() - 1);

    if (_sink)
        _sink->RemoveListener(item);

    VERBOSE(VB_RECORD, LOC + QString("RemoveListener(0x%1) -- end 2")
                       .arg((uint64_t)item,0,16));
}
