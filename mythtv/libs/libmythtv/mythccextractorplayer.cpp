// -*- Mode: c++ -*-

/*
 *  Class MythCCExtractorPlayer
 *
 *  Copyright (C) Digital Nirvana, Inc. 2010
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <iostream>
using namespace std;

#include <QPainter>

#include "teletextextractorreader.h"
#include "mythccextractorplayer.h"
#include "avformatdecoder.h"
#include "srtwriter.h"

SRTStuff::~SRTStuff()
{
    while (!srtwriters.empty())
    {
        delete *srtwriters.begin();
        srtwriters.erase(srtwriters.begin());
    }
}
CC608Stuff::~CC608Stuff() { delete reader; }
CC708Stuff::~CC708Stuff() { delete reader; }
TeletextStuff::~TeletextStuff() { delete reader; }
DVBSubStuff::~DVBSubStuff() { delete reader; }

MythCCExtractorPlayer::MythCCExtractorPlayer(
    bool showProgress, const QString &fileName) :
    MythPlayer(true /*muted*/),
    m_curTime(0),
    m_curTimeShift(-1),
    m_myFramesPlayed(0),
    m_showProgress(showProgress),
    m_fileName(fileName)
{
    // Determine where we will put extracted info.
    QStringList comps = QFileInfo(m_fileName).fileName().split(".");
    if (!comps.empty())
        comps.removeLast();
    m_workingDir = QDir(QFileInfo(m_fileName).path());
    m_baseName = comps.join(".");
}

/**
 * Call it when you got new video frame to process subtitles if any.
 */

void MythCCExtractorPlayer::OnGotNewFrame(void)
{
    m_myFramesPlayed = decoder->GetFramesRead();
    videoOutput->StartDisplayingFrame();
    {
        VideoFrame *frame = videoOutput->GetLastShownFrame();
        if (m_curTimeShift < 0)
            m_curTimeShift = frame->timecode;
        m_curTime = frame->timecode - m_curTimeShift;
        videoOutput->DoneDisplayingFrame(frame);
    }

    Ingest608Captions();  Process608Captions(kProcessNormal);
    Ingest708Captions();  Process708Captions();
    IngestTeletext();     ProcessTeletext();
    IngestDVBSubtitles(); ProcessDVBSubtitles(kProcessNormal);
}

static QString progress_string(
    MythTimer &flagTime, uint64_t m_myFramesPlayed, uint64_t totalFrames)
{
    if (totalFrames == 0ULL)
    {
        return QString("%1 frames processed    \r")
            .arg(m_myFramesPlayed,7);
    }

    double elapsed = flagTime.elapsed() * 0.001;
    double flagFPS = (elapsed > 0.0) ? (m_myFramesPlayed / elapsed) : 0;

    double percentage = m_myFramesPlayed * 100.0 / totalFrames;
    percentage = (percentage > 100.0 && percentage < 101.0) ?
        100.0 : percentage;

    if (flagFPS < 10.0)
    {
        return QString("%1 fps %2%     \r")
            .arg(flagFPS,4,'f',1).arg(percentage,4,'f',1);
    }
    else
    {
        return QString("%1 fps %2%     \r")
            .arg(flagFPS,4,'f',0).arg(percentage,4,'f',1);
    }
}

bool MythCCExtractorPlayer::run(void)
{
    m_myFramesPlayed = 0;

    killdecoder = false;
    framesPlayed = 0;
    using_null_videoout = true;

    decoder->SetDecodeAllSubtitles(true);

    SetPlaying(true);

    if (!InitVideo())
    {
        LOG(VB_GENERAL, LOG_ERR, "Unable to initialize video");
        SetPlaying(false);
        return false;
    }

    ClearAfterSeek();

    MythTimer flagTime, ui_timer, inuse_timer, save_timer;
    flagTime.start();
    ui_timer.start();
    inuse_timer.start();
    save_timer.start();

    m_curTime = 0;

    QString currDir = QFileInfo(m_fileName).path();

    if (DecoderGetFrame(kDecodeVideo))
        OnGotNewFrame();

    if (m_showProgress)
        cout << "\r                                      \r" << flush;

    while (!killdecoder && !IsErrored())
    {
        if (inuse_timer.elapsed() > 2534)
        {
            inuse_timer.restart();
            player_ctx->LockPlayingInfo(__FILE__, __LINE__);
            if (player_ctx->playingInfo)
                player_ctx->playingInfo->UpdateInUseMark();
            player_ctx->UnlockPlayingInfo(__FILE__, __LINE__);
        }

        if (m_showProgress && (ui_timer.elapsed() > 98 * 4))
        {
            ui_timer.restart();
            QString str = progress_string(
                flagTime, m_myFramesPlayed, totalFrames);
            cout << qPrintable(str) << '\r' << flush;
        }

        if (!DecoderGetFrame(kDecodeVideo))
            break;

        OnGotNewFrame();
    }

    if (m_showProgress)
    {
        if ((m_myFramesPlayed < totalFrames) &&
            ((m_myFramesPlayed + 30) > totalFrames))
        {
            m_myFramesPlayed = totalFrames;
        }
        QString str = progress_string(flagTime, m_myFramesPlayed, totalFrames);
        cout << qPrintable(str) << endl;
    }

    Process608Captions(kProcessFinalize);
    ProcessDVBSubtitles(kProcessFinalize);

    SetPlaying(false);
    killdecoder = true;

    return true;
}


/**
 * Adds new subtitle, finishes last if needed.
 * @param content Text content of new subtitle (may be empty).
 * @param list Queue of subtitles we modify.
 */

void MythCCExtractorPlayer::IngestSubtitle(
    QList<OneSubtitle> &list, const QStringList &content)
{
    bool update_last =
        !list.isEmpty() &&
        m_curTime == list.back().start_time &&
        !content.isEmpty();

    if (update_last)
    {
        //update text only (need for cc608)
        list.back().text = content;
        return;
    }

    OneSubtitle last_one = list.isEmpty() ? OneSubtitle() : list.back();
    if (content != last_one.text || last_one.length >= 0)
    {
        // Finish previous subtitle.
        if (!last_one.text.isEmpty() && last_one.length < 0)
        {
            list.back().length = m_curTime - last_one.start_time;
        }

        // Put new one if it isn't empty.
        if (!content.isEmpty())
        {
            OneSubtitle new_one;
            new_one.start_time = m_curTime;
            new_one.text = content;

            list.push_back(new_one);
        }
    }
}

/**
 * Adds new subtitle, finishes last if needed.
 * This is a version for DVB graphical subtitles only.
 * @param content Content of the new subtitle (may be empty).
 * We're going to use it's img & start_time fields.
 */
void MythCCExtractorPlayer::IngestSubtitle(
    QList<OneSubtitle> &list, const OneSubtitle &content)
{
    bool update_last =
        !list.isEmpty() &&
        content.start_time == list.back().start_time &&
        !content.img.isNull();

    if (update_last)
    {
        list.back().img = content.img; // update image only
        return;
    }

    OneSubtitle last_one = list.isEmpty() ? OneSubtitle() : list.back();
    if (content.img != last_one.img || last_one.length >= 0)
    {
        // Finish previous subtitle.
        if (!last_one.img.isNull() && last_one.length < 0)
        {
            list.back().length = content.start_time - last_one.start_time;
        }

        // Put new one if it isn't empty.
        if (!content.img.isNull())
        {
            OneSubtitle new_one;
            new_one.start_time = content.start_time;
            new_one.img = content.img;

            list.push_back(new_one);
        }
    }
}

void MythCCExtractorPlayer::Ingest608Captions(void)
{
    static const int ccIndexTbl[7] =
    {
        0, // CC_CC1
        1, // CC_CC2
        9, // sentinel
        9, // sentinel
        2, // CC_CC3
        3, // CC_CC4
        9, // sentinel
    };

    // for each CC of each video...
    CC608Info::iterator it = m_cc608_info.begin();
    for (; it != m_cc608_info.end(); ++it)
    {
        while (true)
        {
            bool changed = false;
            int streamRawIdx = -1;
            CC608Buffer *textlist = (*it).reader->GetOutputText(
                changed, streamRawIdx);

            if (!changed || !textlist)
                break;

            textlist->lock.lock();

            const int ccIdx = ccIndexTbl[min(streamRawIdx,6)];

            if (ccIdx >= 4)
                continue;

            QStringList content;
            vector<CC608Text*>::iterator bit = textlist->buffers.begin();
            for (; bit != textlist->buffers.end(); ++bit)
                content += CC608Decoder::ToASCII((*bit)->text, true);

            textlist->lock.unlock();

            IngestSubtitle((*it).subs[ccIdx], content);
        }
    }
}

// Note: GetCaptionLanguage() will not return valid if there are multiple videos
void MythCCExtractorPlayer::Process608Captions(uint flags)
{
    int i = 0;
    CC608Info::iterator cc608it = m_cc608_info.begin();
    for (; cc608it != m_cc608_info.end(); ++cc608it)
    {
        QString stream_id_str = (m_cc608_info.size() <= 1) ?
            QString("") : QString("%1.").arg(i,2,QChar('0'));

        CC608StreamType &subs = (*cc608it).subs;
        CC608StreamType::iterator it = subs.begin();
        for (; it != subs.end(); ++it)
        {
            if ((*it).empty())
                continue; // Skip empty subtitle streams.
            if (((kProcessFinalize & flags) == 0) && ((*it).size() <= 1))
                continue; // Leave one caption behind so it can be amended

            int idx = it.key();

            if (!(*cc608it).srtwriters[idx])
            {
                int langCode = 0;
                AvFormatDecoder *avd = dynamic_cast<AvFormatDecoder *>(decoder);
                if (avd)
                    langCode = avd->GetCaptionLanguage(
                        kTrackTypeCC608, idx + 1);

                QString lang = iso639_key_to_str3(langCode);
                lang = iso639_is_key_undefined(langCode) ? "und" : lang;
                
                QString service_key = QString("cc%1").arg(idx + 1);
                QString filename = QString("%1.%2%3-%4.%5.srt")
                    .arg(m_baseName).arg(stream_id_str).arg("608")
                    .arg(service_key).arg(lang);

                (*cc608it).srtwriters[idx] = new SRTWriter(
                    m_workingDir.filePath(filename));
            }

            if (!(*cc608it).srtwriters[idx]->IsOpen())
            {
                (*it).clear();
                continue;
            }

            while ((*it).size() > ((kProcessFinalize & flags) ? 0 : 1))
            {
                (*cc608it).srtwriters[idx]->AddSubtitle(
                    (*it).front(), ++(*cc608it).subs_num[idx]);
                (*it).pop_front();
            }

            (*cc608it).srtwriters[idx]->Flush();
        }
    }
}

void MythCCExtractorPlayer::Ingest708Captions(void)
{
    // For each window of each service of each video...
    CC708Info::const_iterator it = m_cc708_info.begin();
    for (; it != m_cc708_info.end(); ++it)
    {
        for (uint serviceIdx = 1; serviceIdx < 64; ++serviceIdx)
        {
            CC708Service *service = (*it).reader->GetService(serviceIdx);
            for (uint windowIdx = 0; windowIdx < 8; ++windowIdx)
            {
                CC708Window &win = service->windows[windowIdx];
                if (win.changed && win.visible)
                {
                    vector<CC708String*> strings = win.GetStrings();
                    Ingest708Caption(it.key(), serviceIdx, windowIdx, strings);
                    while (!strings.empty())
                    {
                        delete strings.back();
                        strings.pop_back();
                    }
                    service->windows[windowIdx].changed = false;
                }
            }
        }
    }
}

void MythCCExtractorPlayer::Ingest708Caption(
    uint streamId, uint serviceIdx,
    uint windowIdx, const vector<CC708String*> &content)
{
    bool empty = true;
    QStringList winContent;

    vector<CC708String*>::const_iterator it = content.begin();
    for (; it != content.end(); ++it)
    {
        QString tmp = (*it)->str.trimmed();
        if (!tmp.isEmpty())
            winContent += tmp;
        empty &= tmp.isEmpty();
    }

    if (empty)
        return;

    uint key = streamId << 8 | serviceIdx;
    QMap<int, QStringList> &cc708win = m_cc708_windows[key];
    cc708win[windowIdx] = winContent;

    QStringList screenContent;
    QMap<int, QStringList>::const_iterator ccIt = cc708win.begin();
    for (; ccIt != cc708win.end() ; ++ccIt)
        screenContent += *ccIt;

    IngestSubtitle(m_cc708_info[streamId].subs[serviceIdx], screenContent);
}

// Note: GetCaptionLanguage() will not return valid if there are multiple videos
void MythCCExtractorPlayer::Process708Captions(void)
{
    int i = 0;
    CC708Info::iterator cc708it = m_cc708_info.begin();
    for (; cc708it != m_cc708_info.end(); ++cc708it)
    {
        QString stream_id_str = (m_cc708_info.size() <= 1) ?
            QString("") : QString("%1.").arg(i,2,QChar('0'));

        CC708StreamType &subs = (*cc708it).subs;
        CC708StreamType::iterator it = subs.begin();
        for (; it != subs.end(); ++it)
        {
            if ((*it).empty())
                continue; // Skip empty subtitle streams.

            int idx = it.key();

            if (!(*cc708it).srtwriters[idx])
            {
                int langCode = 0;
                AvFormatDecoder *avd = dynamic_cast<AvFormatDecoder*>(decoder);
                if (avd)
                    langCode = avd->GetCaptionLanguage(kTrackTypeCC708, idx);

                QString lang = iso639_key_to_str3(langCode);

                QString service_key = QString("service-%1")
                    .arg(idx, 2, 10, QChar('0'));
                QString id = iso639_is_key_undefined(langCode) ?
                    service_key : lang;
                QString filename = QString("%1.%2%3-%4.%5.srt")
                    .arg(m_baseName).arg(stream_id_str).arg("708")
                    .arg(service_key).arg(lang);

                (*cc708it).srtwriters[idx] = new SRTWriter(
                    m_workingDir.filePath(filename));
            }

            if (!(*cc708it).srtwriters[idx]->IsOpen())
            {
                (*it).clear();
                continue;
            }

            while (!(*it).empty())
            {
                (*cc708it).srtwriters[idx]->AddSubtitle(
                    (*it).front(), ++(*cc708it).subs_num[idx]);
                (*it).pop_front();
            }

            (*cc708it).srtwriters[idx]->Flush();
        }
    }
}

static QStringList to_string_list(const TeletextSubPage &subPage)
{
    QStringList content;
    for (int i = 0; i < 25; ++i)
    {
        QString str = decode_teletext(subPage.lang, subPage.data[i]).trimmed();
        if (!str.isEmpty())
            content += str;
    }
    return content;
}

void MythCCExtractorPlayer::IngestTeletext(void)
{
    TeletextInfo::iterator ttxit = m_ttx_info.begin();
    for (; ttxit != m_ttx_info.end(); ++ttxit)
    {
        typedef QPair<int, int> qpii;
        QSet<qpii> updatedPages = (*ttxit).reader->GetUpdatedPages();
        if (updatedPages.isEmpty())
            continue;

        QSet<qpii>::const_iterator it = updatedPages.constBegin();
        for (; it != updatedPages.constEnd(); ++it)
        {
            (*ttxit).reader->SetPage((*it).first, (*it).second);
            TeletextSubPage *subpage = (*ttxit).reader->FindSubPage();
            if (subpage && subpage->subtitle)
            {
                IngestSubtitle((*ttxit).subs[(*it).first],
                               to_string_list(*subpage));
            }
        }
        
        (*ttxit).reader->ClearUpdatedPages();
    }
}

void MythCCExtractorPlayer::ProcessTeletext(void)
{
    int i = 0;
    TeletextInfo::iterator ttxit = m_ttx_info.begin();
    for (; ttxit != m_ttx_info.end(); ++ttxit)
    {
        QString stream_id_str = (m_cc608_info.size() <= 1) ?
            QString("") : QString("%1.").arg(i,2,QChar('0'));

        TeletextStreamType &subs = (*ttxit).subs;
        TeletextStreamType::iterator it = subs.begin();
        for (; it != subs.end(); ++it)
        {
            if ((*it).empty())
                continue; // Skip empty subtitle streams.

            int page = it.key();

            if (!(*ttxit).srtwriters[page])
            {
                QString filename = QString("%1.%2ttx-0x%3.srt")
                    .arg(m_baseName)
                    .arg(stream_id_str)
                    .arg(page, 3, 16, QChar('0'));

                (*ttxit).srtwriters[page] = new SRTWriter(
                    m_workingDir.filePath(filename));
            }

            if (!(*ttxit).srtwriters[page]->IsOpen())
            {
                (*it).clear();
                continue;
            }

            while (!(*it).empty())
            {
                (*ttxit).srtwriters[page]->AddSubtitle(
                    (*it).front(), ++(*ttxit).subs_num[page]);
                (*it).pop_front();
            }

            (*ttxit).srtwriters[page]->Flush();
        }
    }
}

void MythCCExtractorPlayer::IngestDVBSubtitles(void)
{
    DVBSubInfo::iterator subit = m_dvbsub_info.begin();
    for (; subit != m_dvbsub_info.end(); ++subit)
    {
        /// INFO -- start
        if ((*subit).reader->HasTextSubtitles())
        {
            LOG(VB_VBI, LOG_DEBUG,
                "There are unhandled text dvb subtitles");
        }

        uint64_t duration;
        const QStringList rawSubs =
            (*subit).reader->GetRawTextSubtitles(duration);
        if (!rawSubs.isEmpty())
        {
            LOG(VB_VBI, LOG_DEBUG,
                QString("There are also %1 raw text subtitles with duration %2")
                .arg(rawSubs.size()).arg(duration));
        }
        /// INFO -- end

        AVSubtitles *subtitles = (*subit).reader->GetAVSubtitles();

        QMutexLocker locker(&(subtitles->lock));

        while (!subtitles->buffers.empty())
        {
            const AVSubtitle subtitle = subtitles->buffers.front();
            subtitles->buffers.pop_front();

            const QSize v_size =
                QSize(GetVideoSize().width()*4, GetVideoSize().height()*4); 
            QImage sub_pict(v_size, QImage::Format_ARGB32);
            sub_pict.fill(0);

            int min_x = v_size.width();
            int min_y = v_size.height();
            int max_x = 0;
            int max_y = 0;

            QPainter painter(&sub_pict);
            for (int i = 0; i < (int) subtitle.num_rects; ++i)
            {
                AVSubtitleRect *rect = subtitle.rects[i];

                if (subtitle.rects[i]->type == SUBTITLE_BITMAP)
                {
                    const int x = rect->x;
                    const int y = rect->y;
                    const int w = rect->w;
                    const int h = rect->h;
                    const int cc = rect->nb_colors;
                    const uchar *data = rect->pict.data[0];
                    const QRgb *palette = (QRgb *) rect->pict.data[1];

                    QImage img(data, w, h, QImage::Format_Indexed8);
                    img.setColorCount(cc);
                    for (int i = 0; i < cc; ++i)
                        img.setColor(i, palette[i]);

                    painter.drawImage(x, y, img);

                    min_x = min(min_x, x);
                    min_y = min(min_y, y);
                    max_x = max(max_x, x + w);
                    max_y = max(max_y, y + h);
                }
            }
            painter.end();
            (*subit).reader->FreeAVSubtitle(subtitle);

            OneSubtitle sub;
            sub.start_time = subtitle.start_display_time - m_curTimeShift;
            sub.length =
                subtitle.end_display_time - subtitle.start_display_time;

            if (min_x < max_x && min_y < max_y)
            {
                sub.img_shift = QPoint(min_x, min_y);
                sub.img = sub_pict.copy(
                    min_x, min_y, max_x - min_x, max_y - min_y);
            }
            else
            {
                // Empty subtitle, do nothing.
            }

            IngestSubtitle((*subit).subs, sub);
        }

        locker.unlock();

        (*subit).reader->ClearRawTextSubtitles();
    }
}

void MythCCExtractorPlayer::ProcessDVBSubtitles(uint flags)
{
    // Process (DVB) subtitle streams.
    DVBSubInfo::iterator subit = m_dvbsub_info.begin();
    for (; subit != m_dvbsub_info.end(); ++subit)
    {
        QString dir_name = QString(m_baseName + ".dvb-%1").arg(subit.key());
        if (!m_workingDir.exists(dir_name) && !m_workingDir.mkdir(dir_name))
        {
            LOG(VB_GENERAL, LOG_ERR, QString("Can't create directory '%1'")
                .arg(dir_name));
            (*subit).subs.clear();
            continue;
        }

        DVBStreamType &subs = (*subit).subs;
        if (subs.empty())
            continue; // Skip empty subtitle streams.
        if (((kProcessFinalize & flags) == 0) && (subs.size() <= 1))
            continue; // Leave one caption behind so it can be amended

        QDir stream_dir(m_workingDir.filePath(dir_name));
        while (subs.size() > ((kProcessFinalize & flags) ? 0 : 1))
        {
            const OneSubtitle &sub = subs.front();
            int64_t end_time = sub.start_time + sub.length;
            const QString file_name =
                stream_dir.filePath(
                    QString("%1_%2-to-%3.png")
                    .arg((*subit).subs_num)
                    .arg(sub.start_time).arg(end_time));

            if (end_time > sub.start_time)
            {
                //check is there exist file with same start_time
                QStringList filter;
                filter << QString("*_%1*.png").arg(sub.start_time);
                QFileInfoList found = stream_dir.entryInfoList(filter);
                if (found.isEmpty())
                {
                    //no same start_time founded
                    if (!sub.img.save(file_name))
                    {
                        LOG(VB_GENERAL, LOG_ERR,
                            QString("Can't write file '%1'")
                            .arg(file_name));
                    }
                    (*subit).subs_num++;
                }
            }
            subs.pop_front();
        }
    }
}


CC708Reader *MythCCExtractorPlayer::GetCC708Reader(uint id)
{
    if (!m_cc708_info[id].reader)
    {
        m_cc708_info[id].reader = new CC708Reader(this);
        m_cc708_info[id].reader->SetEnabled(true);
        LOG(VB_GENERAL, LOG_INFO, "Created CC708Reader");
    }
    return m_cc708_info[id].reader;
}

CC608Reader *MythCCExtractorPlayer::GetCC608Reader(uint id)
{
    if (!m_cc608_info[id].reader)
    {
        m_cc608_info[id].reader = new CC608Reader(this);
        m_cc608_info[id].reader->SetEnabled(true);
    }
    return m_cc608_info[id].reader;
}

TeletextReader *MythCCExtractorPlayer::GetTeletextReader(uint id)
{
    if (!m_ttx_info[id].reader)
        m_ttx_info[id].reader = new TeletextExtractorReader();
    return m_ttx_info[id].reader;
}

SubtitleReader *MythCCExtractorPlayer::GetSubReader(uint id)
{
    if (!m_dvbsub_info[id].reader)
    {
        m_dvbsub_info[id].reader = new SubtitleReader();
        m_dvbsub_info[id].reader->EnableAVSubtitles(true);
        m_dvbsub_info[id].reader->EnableTextSubtitles(true);
        m_dvbsub_info[id].reader->EnableRawTextSubtitles(true);
    }
    return m_dvbsub_info[id].reader;
}

