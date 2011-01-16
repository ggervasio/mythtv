#include <QFontMetrics>

#include "mythverbose.h"
#include "mythfontproperties.h"
#include "mythuitext.h"
#include "mythuishape.h"
#include "mythuiimage.h"
#include "mythpainter.h"
#include "subtitlescreen.h"
#include "bdringbuffer.h"

#define LOC      QString("Subtitles: ")
#define LOC_WARN QString("Subtitles Warning: ")
#define PAD_WIDTH  0.20
#define PAD_HEIGHT 0.04

static MythFontProperties* gTextSubFont;
static QHash<int, MythFontProperties*> gCC708Fonts;

SubtitleScreen::SubtitleScreen(MythPlayer *player, const char * name,
                               int fontStretch) :
    MythScreenType((MythScreenType*)NULL, name),
    m_player(player),  m_subreader(NULL),   m_608reader(NULL),
    m_708reader(NULL), m_safeArea(QRect()), m_useBackground(false),
    m_608fontZoom(100),
    m_teletextmode(false), m_xmid(0),       m_yoffset(0),
    m_fontwidth(100),
    m_removeHTML(QRegExp("</?.+>")),        m_subtitleType(kDisplayNone),
    m_subtitleZoom(100),
    m_textFontZoom(100),                    m_refreshArea(false),
    m_fontStretch(fontStretch)
{
    m_708fontSizes[0] = 36;
    m_708fontSizes[1] = 45;
    m_708fontSizes[2] = 60;
    m_removeHTML.setMinimal(true);
}

SubtitleScreen::~SubtitleScreen(void)
{
    ClearAllSubtitles();
}

void SubtitleScreen::EnableSubtitles(int type)
{
    m_subtitleType = type;
    if (m_subreader)
    {
        m_subreader->EnableAVSubtitles(kDisplayAVSubtitle == m_subtitleType);
        m_subreader->EnableTextSubtitles(kDisplayTextSubtitle == m_subtitleType);
        m_subreader->EnableRawTextSubtitles(kDisplayRawTextSubtitle == m_subtitleType);
    }
    if (m_608reader)
        m_608reader->SetEnabled(kDisplayCC608 == m_subtitleType);
    if (m_708reader)
        m_708reader->SetEnabled(kDisplayCC708 == m_subtitleType);
    ClearAllSubtitles();
    SetVisible(m_subtitleType != kDisplayNone);
    SetArea(MythRect());
}

bool SubtitleScreen::Create(void)
{
    if (!m_player)
        return false;

    m_subreader = m_player->GetSubReader();
    m_608reader = m_player->GetCC608Reader();
    m_708reader = m_player->GetCC708Reader();
    if (!m_subreader)
        VERBOSE(VB_IMPORTANT, LOC_WARN + "Failed to get subtitle reader.");
    if (!m_608reader)
        VERBOSE(VB_IMPORTANT, LOC_WARN + "Failed to get CEA-608 reader.");
    if (!m_708reader)
        VERBOSE(VB_IMPORTANT, LOC_WARN + "Failed to get CEA-708 reader.");
    m_subtitleZoom  = gCoreContext->GetNumSetting("OSDSubtitleTextZoom", 100);
    m_useBackground = (bool)gCoreContext->GetNumSetting("CCBackground", 0);
    m_608fontZoom   = gCoreContext->GetNumSetting("OSDCC608TextZoom", 100);
    m_textFontZoom  = gCoreContext->GetNumSetting("OSDCC708TextZoom", 100);
    return true;
}

void SubtitleScreen::Pulse(void)
{
    ExpireSubtitles();
    if (kDisplayAVSubtitle == m_subtitleType)
        DisplayAVSubtitles();
    else if (kDisplayTextSubtitle == m_subtitleType)
        DisplayTextSubtitles();
    else if (kDisplayCC608 == m_subtitleType)
        DisplayCC608Subtitles();
    else if (kDisplayCC708 == m_subtitleType)
        DisplayCC708Subtitles();
    else if (kDisplayRawTextSubtitle == m_subtitleType)
        DisplayRawTextSubtitles();

    OptimiseDisplayedArea();
    m_refreshArea = false;
}

void SubtitleScreen::ClearAllSubtitles(void)
{
    ClearNonDisplayedSubtitles();
    ClearDisplayedSubtitles();
}

void SubtitleScreen::ClearNonDisplayedSubtitles(void)
{
    if (m_subreader && (kDisplayAVSubtitle == m_subtitleType))
        m_subreader->ClearAVSubtitles();
    if (m_subreader && (kDisplayRawTextSubtitle == m_subtitleType))
        m_subreader->ClearRawTextSubtitles();
    if (m_608reader && (kDisplayCC608 == m_subtitleType))
        m_608reader->ClearBuffers(true, true);
    if (m_708reader && (kDisplayCC708 == m_subtitleType))
        m_708reader->ClearBuffers();
}

void SubtitleScreen::ClearDisplayedSubtitles(void)
{
    for (int i = 0; i < 8; i++)
        Clear708Cache(i);
    DeleteAllChildren();
    m_expireTimes.clear();
    SetRedraw();
}

void SubtitleScreen::ExpireSubtitles(void)
{
    VideoOutput    *videoOut = m_player->getVideoOutput();
    VideoFrame *currentFrame = videoOut ? videoOut->GetLastShownFrame() : NULL;
    long long now = currentFrame ? currentFrame->timecode : LLONG_MAX;
    QMutableHashIterator<MythUIType*, long long> it(m_expireTimes);
    while (it.hasNext())
    {
        it.next();
        if (it.value() < now)
        {
            DeleteChild(it.key());
            it.remove();
            SetRedraw();
        }
    }
}

void SubtitleScreen::OptimiseDisplayedArea(void)
{
    if (!m_refreshArea)
        return;

    QRegion visible;
    QListIterator<MythUIType *> i(m_ChildrenList);
    while (i.hasNext())
    {
        MythUIType *img = i.next();
        visible = visible.united(img->GetArea());
    }

    if (visible.isEmpty())
        return;

    QRect bounding  = visible.boundingRect();
    bounding = bounding.translated(m_safeArea.topLeft());
    bounding = m_safeArea.intersected(bounding);
    int left = m_safeArea.left() - bounding.left();
    int top  = m_safeArea.top()  - bounding.top();
    SetArea(MythRect(bounding));

    i.toFront();;
    while (i.hasNext())
    {
        MythUIType *img = i.next();
        img->SetArea(img->GetArea().translated(left, top));
    }
}

void SubtitleScreen::DisplayAVSubtitles(void)
{
    if (!m_player || !m_subreader)
        return;

    VideoOutput    *videoOut = m_player->getVideoOutput();
    VideoFrame *currentFrame = videoOut ? videoOut->GetLastShownFrame() : NULL;

    if (!currentFrame || !videoOut)
        return;

    float tmp = 0.0;
    QRect dummy;
    videoOut->GetOSDBounds(dummy, m_safeArea, tmp, tmp, tmp);

    AVSubtitles* subs = m_subreader->GetAVSubtitles();
    subs->lock.lock();
    while (!subs->buffers.empty())
    {
        const AVSubtitle subtitle = subs->buffers.front();
        if (subtitle.start_display_time > currentFrame->timecode)
            break;

        long long displayfor = subtitle.end_display_time -
                               subtitle.start_display_time;
        if (displayfor == 0)
            displayfor = 60000;
        displayfor = (displayfor < 50) ? 50 : displayfor;
        long long late = currentFrame->timecode -
                         subtitle.start_display_time;

        ClearDisplayedSubtitles();
        subs->buffers.pop_front();
        for (std::size_t i = 0; i < subtitle.num_rects; ++i)
        {
            AVSubtitleRect* rect = subtitle.rects[i];

            bool displaysub = true;
            if (subs->buffers.size() > 0 &&
                subs->buffers.front().end_display_time <
                currentFrame->timecode)
            {
                displaysub = false;
            }

            if (displaysub && rect->type == SUBTITLE_BITMAP)
            {
                QRect display(rect->display_x, rect->display_y,
                              rect->display_w, rect->display_h);

                // XSUB and some DVD/DVB subs are based on the original video
                // size before the video was converted. We need to guess the
                // original size and allow for the difference

                int right  = rect->x + rect->w;
                int bottom = rect->y + rect->h;
                if (subs->fixPosition || (currentFrame->height < bottom) ||
                   (currentFrame->width  < right) ||
                   !display.width() || !display.height())
                {
                    int sd_height = 576;
                    if ((m_player->GetFrameRate() > 26.0f) && bottom <= 480)
                        sd_height = 480;
                    int height = ((currentFrame->height <= sd_height) &&
                                  (bottom <= sd_height)) ? sd_height :
                                 ((currentFrame->height <= 720) && bottom <= 720)
                                   ? 720 : 1080;
                    int width  = ((currentFrame->width  <= 720) &&
                                  (right <= 720)) ? 720 :
                                 ((currentFrame->width  <= 1280) &&
                                  (right <= 1280)) ? 1280 : 1920;
                    display = QRect(0, 0, width, height);
                }

                // split into upper/lower to allow zooming
                QRect bbox;
                int uh = display.height()/2 - rect->y;
                int lh;
                if (uh > 0)
                {
                    bbox = QRect(0, 0, rect->w, uh);
                    uh = DisplayScaledAVSubtitles(rect, bbox, true, display,
                                                  currentFrame->timecode + displayfor, late);
                }
                else
                    uh = 0;
                lh = rect->h - uh;
                if (lh > 0)
                {
                    bbox = QRect(0, uh, rect->w, lh);
                    lh = DisplayScaledAVSubtitles(rect, bbox, false, display,
                                                  currentFrame->timecode + displayfor, late);
                }
            }
        }
        m_subreader->FreeAVSubtitle(subtitle);
    }
    subs->lock.unlock();
}

int SubtitleScreen::DisplayScaledAVSubtitles(const AVSubtitleRect *rect, QRect &bbox,
                                             bool top, QRect &display,
                                             long long displayuntil, long long late)
{
    // crop image to reduce scaling time
    int xmin, xmax, ymin, ymax;
    int ylast, ysplit;
    bool prev_empty = false;

    // initialize to opposite edges
    xmin = bbox.right();
    xmax = bbox.left();
    ymin = bbox.bottom();
    ymax = bbox.top();
    ylast = bbox.top();
    ysplit = bbox.bottom();

    // find bounds of active image
    for (int y = bbox.top(); y <= bbox.bottom(); ++y)
    {
        if (y >= rect->h)
        {
            // end of image
            if (!prev_empty)
                ylast = y;
            break;
        }

        bool empty = true;
        for (int x = bbox.left(); x <= bbox.right(); ++x)
        {
            const uint8_t color = rect->pict.data[0][y*rect->pict.linesize[0] + x];
            const uint32_t pixel = *((uint32_t *)rect->pict.data[1]+color);
            if (pixel & 0xff000000)
            {
                empty = false;
                if (x < xmin)
                    xmin = x;
                if (x > xmax)
                    xmax = x;
            }
        }

        if (!empty)
        {
            if (y < ymin)
                ymin = y;
            if (y > ymax)
                ymax = y;
        }
        else if (!prev_empty)
        {
            // remember uppermost empty line
            ylast = y;
        }
        prev_empty = empty;
    }

    if (ymax <= ymin)
        return 0;

    if (top)
    {
        if (ylast < ymin)
            // no empty lines
            return 0;

        if (ymax == bbox.bottom())
        {
            ymax = ylast;
            ysplit = ylast;
        }
    }

    // set new bounds
    bbox.setLeft(xmin);
    bbox.setRight(xmax);
    bbox.setTop(ymin);
    bbox.setBottom(ymax);

    // copy active region
    // AVSubtitleRect's image data's not guaranteed to be 4 byte
    // aligned.

    QRect orig_rect(bbox.left(), bbox.top(), bbox.width(), bbox.height());

    QImage qImage(bbox.width(), bbox.height(), QImage::Format_ARGB32);
    for (int y = 0; y < bbox.height(); ++y)
    {
        int ysrc = y + bbox.top();
        for (int x = 0; x < bbox.width(); ++x)
        {
            int xsrc = x + bbox.left();
            const uint8_t color = rect->pict.data[0][ysrc*rect->pict.linesize[0] + xsrc];
            const uint32_t pixel = *((uint32_t *)rect->pict.data[1]+color);
            qImage.setPixel(x, y, pixel);
        }
    }

    // translate to absolute coordinates
    bbox.translate(rect->x, rect->y);

    // scale and move according to zoom factor
    int zoom = m_subtitleZoom;
    bbox.setWidth(bbox.width() * zoom/100);
    bbox.setHeight(bbox.height() * zoom/100);

    VideoOutput *videoOut = m_player->getVideoOutput();
    QRect scaled = videoOut->GetImageRect(bbox, &display);

    if (scaled.size() != orig_rect.size())
        qImage = qImage.scaled(scaled.width(), scaled.height(),
                               Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

    int hsize = m_safeArea.width();
    int vsize = m_safeArea.height();

    scaled.moveLeft(((100-zoom)*hsize/2 + zoom*scaled.left())/100);
    if (top)
        // clamp up
        scaled.moveTop(scaled.top() * zoom/100);
    else
        // clamp down
        scaled.moveTop(((100-zoom)*vsize + zoom*scaled.top())/100);


    MythPainter *osd_painter = videoOut->GetOSDPainter();
    MythImage* image = NULL;
    if (osd_painter)
       image = osd_painter->GetFormatImage();

    MythUIImage *uiimage = NULL;
    if (image)
    {
        image->Assign(qImage);
        QString name = QString("avsub");
        uiimage = new MythUIImage(this, name);
        if (uiimage)
        {
            m_refreshArea = true;
            uiimage->SetImage(image);
            uiimage->SetArea(MythRect(scaled));
            m_expireTimes.insert(uiimage, displayuntil);
        }
    }
    if (uiimage)
    {
        VERBOSE(VB_PLAYBACK, LOC +
            QString("Display AV sub until %1 ms").arg(displayuntil));
        if (late > 50)
            VERBOSE(VB_PLAYBACK, LOC +
                QString("AV Sub was %1 ms late").arg(late));
    }

    return (ysplit+1);
}

void SubtitleScreen::DisplayTextSubtitles(void)
{
    if (!InitialiseFont(m_fontStretch) || !m_player || !m_subreader)
        return;

    bool changed = false;
    VideoOutput *vo = m_player->getVideoOutput();
    if (vo)
    {
        QRect oldsafe = m_safeArea;
        m_safeArea = m_player->getVideoOutput()->GetSafeRect();
        if (oldsafe != m_safeArea)
        {
            changed = true;
            int height = (m_safeArea.height() * m_textFontZoom) / 1800;
            gTextSubFont->GetFace()->setPixelSize(height);
            gTextSubFont->SetColor(Qt::white);
        }
    }
    else
    {
        return;
    }

    VideoFrame *currentFrame = vo->GetLastShownFrame();
    if (!currentFrame)
        return;

    TextSubtitles *subs = m_subreader->GetTextSubtitles();
    subs->Lock();
    uint64_t playPos = 0;
    if (subs->IsFrameBasedTiming())
    {
        // frame based subtitles get out of synch after running mythcommflag
        // for the file, i.e., the following number is wrong and does not
        // match the subtitle frame numbers:
        playPos = currentFrame->frameNumber;
    }
    else
    {
        // Use timecodes for time based SRT subtitles. Feeding this into
        // NormalizeVideoTimecode() should adjust for non-zero start times
        // and wraps. For MPEG, wraps will occur just once every 26.5 hours
        // and other formats less frequently so this should be sufficient.
        // Note: timecodes should now always be valid even in the case
        // when a frame doesn't have a valid timestamp. If an exception is
        // found where this is not true then we need to use the frameNumber
        // when timecode is not defined by uncommenting the following lines.
        //if (currentFrame->timecode == 0)
        //    playPos = (uint64_t)
        //        ((currentFrame->frameNumber / video_frame_rate) * 1000);
        //else
        playPos = m_player->GetDecoder()->NormalizeVideoTimecode(currentFrame->timecode);
    }
    if (playPos != 0)
        changed |= subs->HasSubtitleChanged(playPos);
    if (!changed)
    {
        subs->Unlock();
        return;
    }

    DeleteAllChildren();
    SetRedraw();
    if (playPos == 0)
    {
        subs->Unlock();
        return;
    }

    QStringList rawsubs = subs->GetSubtitles(playPos);
    if (rawsubs.empty())
    {
        subs->Unlock();
        return;
    }

    OptimiseTextSubs(rawsubs);
    subs->Unlock();
    DrawTextSubtitles(rawsubs, 0, 0);
}

void SubtitleScreen::DisplayRawTextSubtitles(void)
{
    if (!InitialiseFont(m_fontStretch) || !m_player || !m_subreader)
        return;

    uint64_t duration;
    QStringList subs = m_subreader->GetRawTextSubtitles(duration);
    if (subs.empty())
        return;

    VideoOutput *vo = m_player->getVideoOutput();
    if (vo)
    {
        QRect oldsafe = m_safeArea;
        m_safeArea = m_player->getVideoOutput()->GetSafeRect();
        if (oldsafe != m_safeArea)
        {
            int height = (m_safeArea.height() * m_textFontZoom) / 1800;
            gTextSubFont->GetFace()->setPixelSize(height);
            gTextSubFont->SetColor(Qt::white);
        }
    }
    else
        return;

    VideoFrame *currentFrame = vo->GetLastShownFrame();
    if (!currentFrame)
        return;

    // delete old subs that may still be on screen
    DeleteAllChildren();
    OptimiseTextSubs(subs);
    DrawTextSubtitles(subs, currentFrame->timecode, duration);
}

void SubtitleScreen::OptimiseTextSubs(QStringList &rawsubs)
{
    QFontMetrics font(*(gTextSubFont->GetFace()));
    int maxwidth = m_safeArea.width();
    QStringList wrappedsubs;
    QString wrappedtext = "";
    int i = 0;
    while ((i < rawsubs.size()) || !wrappedtext.isEmpty())
    {
        QString nextline = wrappedtext;
        if (i < rawsubs.size())
            nextline += rawsubs[i].remove((const QRegExp&) m_removeHTML);
        wrappedtext = "";

        while (font.width(nextline) > maxwidth)
        {
            QString word = nextline.section(" ", -1, -1,
                                            QString::SectionSkipEmpty);
            if (word.isEmpty() || font.width(word) > maxwidth)
                break;
            wrappedtext = word + " " + wrappedtext;
            nextline.chop(word.size() + 1);
        }
        if (!nextline.isEmpty())
            wrappedsubs.append(nextline);
        i++;
    }
    rawsubs = wrappedsubs;
}

void SubtitleScreen::DrawTextSubtitles(QStringList &wrappedsubs,
                                       uint64_t start, uint64_t duration)
{
    QFontMetrics font(*(gTextSubFont->GetFace()));
    int height = font.height() * (1 + PAD_HEIGHT);
    int pad_width = font.maxWidth() * PAD_WIDTH;
    int y = m_safeArea.height() - (height * wrappedsubs.size());
    int centre = m_safeArea.width() / 2;
    QBrush bgfill = QBrush(QColor(0, 0, 0), Qt::SolidPattern);
    foreach (QString subtitle, wrappedsubs)
    {
        if (subtitle.isEmpty())
            continue;
        int width = font.width(subtitle) + pad_width * 2;
        int x = centre - (width / 2) - pad_width;
        QRect rect(x, y, width, height);

        if (m_useBackground)
        {
            MythUIShape *shape = new MythUIShape(this,
                QString("tsubbg%1%2").arg(x).arg(y));
            shape->SetFillBrush(bgfill);
            shape->SetArea(MythRect(rect));
            if (duration > 0)
                m_expireTimes.insert(shape, start + duration);
        }
        MythUIText* text = new MythUIText(subtitle, *gTextSubFont, rect,
                                rect, this, QString("tsub%1%2").arg(x).arg(y));
        if (text)
            text->SetJustification(Qt::AlignCenter);
        y += height;
        VERBOSE(VB_PLAYBACK, LOC + subtitle);
        m_refreshArea = true;

        if (duration > 0)
        {
            m_expireTimes.insert(text, start + duration);
            VERBOSE(VB_PLAYBACK, LOC +
                QString("Display text subtitle for %1 ms").arg(duration));
        }
    }
}

void SubtitleScreen::DisplayDVDButton(AVSubtitle* dvdButton, QRect &buttonPos)
{
    if (!dvdButton || !m_player)
        return;

    VideoOutput *vo = m_player->getVideoOutput();
    if (!vo)
        return;

    DeleteAllChildren();
    SetRedraw();

    float tmp = 0.0;
    QRect dummy;
    vo->GetOSDBounds(dummy, m_safeArea, tmp, tmp, tmp);

    AVSubtitleRect *hl_button = dvdButton->rects[0];
    uint h = hl_button->h;
    uint w = hl_button->w;
    QRect rect = QRect(hl_button->x, hl_button->y, w, h);
    QImage bg_image(hl_button->pict.data[0], w, h, QImage::Format_Indexed8);
    uint32_t *bgpalette = (uint32_t *)(hl_button->pict.data[1]);

    bool blank = true;
    for (uint x = 0; (x < w) && bgpalette; x++)
    {
        for (uint y = 0; y < h; y++)
        {
            if (qAlpha(bgpalette[bg_image.pixelIndex(x, y)]) > 0)
            {
                blank = false;
                break;
            }
        }
    }

    if (!blank)
    {
        QVector<unsigned int> bg_palette;
        for (int i = 0; i < AVPALETTE_COUNT; i++)
            bg_palette.push_back(bgpalette[i]);
        bg_image.setColorTable(bg_palette);
        bg_image = bg_image.convertToFormat(QImage::Format_ARGB32);
        AddScaledImage(bg_image, rect);
        VERBOSE(VB_PLAYBACK, LOC + "Added DVD button background");
    }

    QImage fg_image = bg_image.copy(buttonPos);
    QVector<unsigned int> fg_palette;
    uint32_t *fgpalette = (uint32_t *)(dvdButton->rects[1]->pict.data[1]);
    if (fgpalette)
    {
        for (int i = 0; i < AVPALETTE_COUNT; i++)
            fg_palette.push_back(fgpalette[i]);
        fg_image.setColorTable(fg_palette);
    }

    // scale highlight image to match OSD size, if required
    QRect button = buttonPos.adjusted(0, 2, 0, 0);
    fg_image = fg_image.convertToFormat(QImage::Format_ARGB32);
    AddScaledImage(fg_image, button);
}

void SubtitleScreen::DisplayBDOverlay(BDOverlay *overlay)
{
    if (!overlay || !m_player)
        return;

    VideoOutput *vo = m_player->getVideoOutput();
    if (!vo)
        return;

    // set the screen area
    float tmp = 0.0;
    QRect dummy;
    vo->GetOSDBounds(dummy, m_safeArea, tmp, tmp, tmp);

    // convert the palette to ARGB
    uint32_t *origpalette = (uint32_t *)(overlay->m_palette);
    QVector<unsigned int> palette;
    for (int i = 0; i < 256; i++)
    {
        int y  = (origpalette[i] >> 0) & 0xff;
        int cr = (origpalette[i] >> 8) & 0xff;
        int cb = (origpalette[i] >> 16) & 0xff;
        int a  = (origpalette[i] >> 24) & 0xff;
        int r  = int(y + 1.4022 * (cr - 128));
        int b  = int(y + 1.7710 * (cb - 128));
        int g  = int(1.7047 * y - (0.1952 * b) - (0.5647 * r));
        if (r < 0) r = 0;
        if (g < 0) g = 0;
        if (b < 0) b = 0;
        if (r > 0xff) r = 0xff;
        if (g > 0xff) g = 0xff;
        if (b > 0xff) b = 0xff;
        palette.push_back((a << 24) | (r << 16) | (g << 8) | b);
    }

    // convert the image to QImage
    QImage img(overlay->m_position.size(), QImage::Format_Indexed8);
    memcpy(img.bits(), overlay->m_data,
           overlay->m_position.width() * overlay->m_position.height());
    img.setColorTable(palette);
    QImage converted = img.convertToFormat(QImage::Format_ARGB32);

    // add to screen
    AddScaledImage(converted, overlay->m_position);
    SetRedraw();
    m_refreshArea = false; // N.B. this disables some optimisations

    // release the overlay
    delete overlay->m_data;
    delete overlay->m_palette;
    delete overlay;
}

void SubtitleScreen::SetFontParams(void)
{
    //int xscale = m_teletextmode ? 40 : 36;
    int yscale = m_teletextmode ? 25 : 17;
    gTextSubFont->GetFace()->setPixelSize(m_safeArea.height() * m_608fontZoom
                                          / (yscale * 1.2 * 100));
    m_xmid = m_safeArea.width()/2;
    m_yoffset = 0;

    QFontMetrics font(*(gTextSubFont->GetFace()));
    m_fontwidth = font.averageCharWidth();
    VERBOSE(VB_PLAYBACK, LOC + QString("xmid = %1, fontwidth = %4").
                                       arg(m_xmid).arg(m_fontwidth));
}

void SubtitleScreen::DisplayCC608Subtitles(void)
{
    static const QColor clr[8] =
    {
        Qt::lightGray,   Qt::red,     Qt::green, Qt::yellow,
        Qt::blue,    Qt::magenta, Qt::cyan,  Qt::white,
    };

    if (!InitialiseFont(m_fontStretch) || !m_608reader)
        return;

    bool changed = false;

    if (m_player && m_player->getVideoOutput())
    {
        QRect oldsafe = m_safeArea;
        m_safeArea = m_player->getVideoOutput()->GetSafeRect();
        if (oldsafe != m_safeArea || m_xmid == 0)
        {
            changed = true;
            SetFontParams();
        }
    }
    else
    {
        return;
    }

    CC608Buffer* textlist = m_608reader->GetOutputText(changed);
    if (!changed)
        return;
    if (textlist)
        textlist->lock.lock();
    DeleteAllChildren();
    if (!textlist)
        return;
    if (textlist && textlist->buffers.empty())
    {
        SetRedraw();
        textlist->lock.unlock();
        return;
    }

    vector<CC608Text*>::iterator i = textlist->buffers.begin();
    bool teletextmode = (*i)->teletextmode;
    if (teletextmode != m_teletextmode)
    {
        m_teletextmode = teletextmode;
        SetFontParams();
    }
    QFontMetrics font(*(gTextSubFont->GetFace()));
    QBrush bgfill = QBrush(QColor(0, 0, 0), Qt::SolidPattern);
    int height = font.height() * (1 + PAD_HEIGHT);
    int pad_width = font.maxWidth() * PAD_WIDTH;

    for (; i != textlist->buffers.end(); i++)
    {
        CC608Text *cc = (*i);

        if (cc && (cc->text != QString::null))
        {
            int width = font.width(cc->text) + pad_width;
            int x0, y0;
            int x, y;
            int rows, cols;

            if (cc->teletextmode)
            {
                // teletext expects a 24 row / 40 char grid
                rows = 24+1;
                cols = 40;
                x0 = cc->y;
                y0 = cc->x;
            }
            else
            {
                // CC has 15 rows, 32 columns
                // - add one row top and bottom for margin
                rows = 15+2;
                cols = 32;
                x0 = cc->x;  // +3 ?
                y0 = cc->y;
            }

            // position as if we use a fixed size font
            // - font size already has zoom factor applied

            // center horizontally
            x = m_xmid + (x0 - cols/2) * m_fontwidth;

            if (y0 < rows/2)
                // top half -- clamp up
                y = m_yoffset + (y0 * m_safeArea.height() * m_608fontZoom / (rows * 100));
            else
                // bottom half -- clamp down
                y = m_yoffset + m_safeArea.height()
                    - ((rows - y0) * m_safeArea.height() * m_608fontZoom / (rows * 100));

            //int maxx = x + width;
            //int maxy = y + font.height() * 3 / 2;
            //
            //if (maxx > surface->width)
            //    maxx = surface->width;
            //
            //if (maxy > surface->height)
            //    maxy = surface->height;

            QRect rect(x, y, width, height);

            if (!teletextmode && m_useBackground)
            {
                MythUIShape *shape = new MythUIShape(this,
                    QString("cc608bg%1%2%3").arg(cc->x).arg(cc->y).arg(width));
                shape->SetFillBrush(bgfill);
                QRect bgrect(x - pad_width, y, width + pad_width, height);
                shape->SetArea(MythRect(bgrect));
            }

            gTextSubFont->SetColor(clr[min(max(0, cc->color), 7)]);
            MythUIText *text = new MythUIText(
                   cc->text, *gTextSubFont, rect, rect, (MythUIType*)this,
                   QString("cc608txt%1%2%3").arg(cc->x).arg(cc->y).arg(width));
            if (text)
                text->SetJustification(Qt::AlignLeft);
            m_refreshArea = true;
            VERBOSE(VB_VBI, QString("x %1 y %2 String: '%3'")
                                .arg(cc->x).arg(cc->y).arg(cc->text));
        }
    }
    textlist->lock.unlock();
}

void SubtitleScreen::DisplayCC708Subtitles(void)
{
    if (!m_708reader)
        return;

    CC708Service *cc708service = m_708reader->GetCurrentService();
    float video_aspect = 1.77777f;
    if (m_player && m_player->getVideoOutput())
    {
        video_aspect = m_player->GetVideoAspect();
        QRect oldsafe = m_safeArea;
        m_safeArea = m_player->getVideoOutput()->GetSafeRect();
        if (oldsafe != m_safeArea)
        {
            for (uint i = 0; i < 8; i++)
                cc708service->windows[i].changed = true;
            int size = (m_safeArea.height() * m_textFontZoom) / 2000;
            m_708fontSizes[1] = size;
            m_708fontSizes[0] = size * 32 / 42;
            m_708fontSizes[2] = size * 42 / 32;
        }
    }
    else
    {
        return;
    }

    if (!Initialise708Fonts(m_fontStretch))
        return;

    for (uint i = 0; i < 8; i++)
    {
        CC708Window &win = cc708service->windows[i];
        if (win.exists && win.visible && !win.changed)
            continue;

        Clear708Cache(i);
        if (!win.exists || !win.visible)
            continue;

        QMutexLocker locker(&win.lock);
        vector<CC708String*> list = win.GetStrings();
        if (list.size())
            Display708Strings(win, i, video_aspect, list);
        for (uint j = 0; j < list.size(); j++)
            delete list[j];
        win.changed = false;
    }
}

void SubtitleScreen::Clear708Cache(int num)
{
    if (!m_708imageCache[num].isEmpty())
    {
        foreach(MythUIType* image, m_708imageCache[num])
            DeleteChild(image);
        m_708imageCache[num].clear();
    }
}

void SubtitleScreen::Display708Strings(const CC708Window &win, int num,
                                       float aspect, vector<CC708String*> &list)
{
    VERBOSE(VB_VBI, LOC + QString("Display Win %1, Anchor_id %2, x_anch %3, "
                                  "y_anch %4, relative %5")
                .arg(num).arg(win.anchor_point).arg(win.anchor_horizontal)
                .arg(win.anchor_vertical).arg(win.relative_pos));

    bool display = false;
    MythFontProperties *mythfont;
    uint max_row_width = 0;
    uint total_height = 0;
    uint i = 0;
    for (uint row = 0; (row < win.true_row_count) && (i < list.size()); row++)
    {
        uint row_width = 0, max_row_height = 0;
        for (; (i < list.size()) && list[i] && (list[i]->y <= row); i++)
        {
            if (list[i]->y < row)
                continue;

            mythfont = Get708Font(list[i]->attr);
            if (!mythfont)
                continue;

            QString text = list[i]->str.trimmed();
            if (!text.isEmpty())
                display = true;

            QFontMetrics font(*(mythfont->GetFace()));
            uint height = (uint)font.height() * (1 + PAD_HEIGHT);

            row_width += font.width(list[i]->str) +
                         (font.maxWidth() * PAD_WIDTH * 2);
            max_row_height = max(max_row_height, height);
        }

        max_row_width = max(max_row_width, row_width);
        total_height += max_row_height;
    }

    if (!display)
        return;

    float xrange  = win.relative_pos ? 100.0f :
                    (aspect > 1.4f) ? 210.0f : 160.0f;
    float yrange  = win.relative_pos ? 100.0f : 75.0f;
    float xmult   = (float)m_safeArea.width() / xrange;
    float ymult   = (float)m_safeArea.height() / yrange;
    uint anchor_x = (uint)(xmult * (float)win.anchor_horizontal);
    uint anchor_y = (uint)(ymult * (float)win.anchor_vertical);

    if (win.anchor_point % 3 == 1)
        anchor_x -= (((int)max_row_width) / 2);
    if (win.anchor_point % 3 == 2)
        anchor_x -= (int)max_row_width;
    if (win.anchor_point / 3 == 1)
        anchor_y -= (((int)total_height) / 2);
    if (win.anchor_point / 3 == 2)
        anchor_y -= (int)total_height;

    if (win.GetFillAlpha()) // TODO border?
    {
        QRect bg(anchor_x, anchor_y, max_row_width, total_height);
        QBrush fill(win.GetFillColor(), Qt::SolidPattern);
        MythUIShape *shape = new MythUIShape(this,
                QString("cc708bg%1").arg(num));
        shape->SetFillBrush(fill);
        shape->SetArea(MythRect(bg));
        m_708imageCache[num].append(shape);
        m_refreshArea = true;
    }

    i = 0;
    int y = anchor_y;
    for (uint row = 0; (row < win.true_row_count) && (i < list.size()); row++)
    {
        uint maxheight = 0;
        int  x = anchor_x;
        bool first = true;
        for (; (i < list.size()) && list[i] && (list[i]->y <= row); i++)
        {
            bool last = ((i + 1) == list.size());
            if (!last)
                last = (list[i + 1]->y > row);

            QString rawstring = list[i]->str;
            mythfont = Get708Font(list[i]->attr);

            if ((list[i]->y < row) || !mythfont || rawstring.isEmpty())
                continue;

            QString trimmed = rawstring.trimmed();
            if (!trimmed.size() && last)
                continue;

            QFontMetrics font(*(mythfont->GetFace()));
            uint height = (uint)font.height() * (1 + PAD_HEIGHT);
            maxheight   = max(maxheight, height);
            uint spacewidth = font.width(QString(" "));
            uint textwidth  = font.width(trimmed);

            int leading  = 0;
            int trailing = 0;
            if (trimmed.size() != rawstring.size())
            {
                if (trimmed.size())
                {
                    leading  = rawstring.indexOf(trimmed.at(0));
                    trailing = rawstring.size() - trimmed.size() - leading;
                }
                else
                {
                    leading = rawstring.size();
                }
                leading  *= spacewidth;
                trailing *= spacewidth;
            }

            if (!leading)
                textwidth += spacewidth * PAD_WIDTH;
            if (!trailing)
                textwidth += spacewidth * PAD_WIDTH;

            bool background = list[i]->attr.GetBGAlpha();
            QBrush bgfill = QBrush((list[i]->attr.GetBGColor(), Qt::SolidPattern));

            if (leading && background && !first)
            {
                // draw background for leading space
                QRect space(x, y, leading, height);
                MythUIShape *shape = new MythUIShape(this,
                        QString("cc708shape%1x%2lead").arg(row).arg(i));
                shape->SetFillBrush(bgfill);
                shape->SetArea(MythRect(space));
                m_708imageCache[num].append(shape);
                m_refreshArea = true;
            }

            x += leading;
            QRect rect(x, y, textwidth, height);

            if (trimmed.size() && textwidth && background)
            {
                MythUIShape *shape = new MythUIShape(this,
                        QString("cc708shape%1x%2main").arg(row).arg(i));
                shape->SetFillBrush(bgfill);
                shape->SetArea(MythRect(rect));
                m_708imageCache[num].append(shape);
                m_refreshArea = true;
            }

            if (trimmed.size() && textwidth)
            {
                MythUIText *text = new MythUIText(list[i]->str, *mythfont,
                                                  rect, rect,
                                                  (MythUIType*)this,
                                                  QString("cc708text%1x%2").arg(row).arg(i));
                m_708imageCache[num].append(text);
                if (text)
                    text->SetJustification(Qt::AlignCenter);
                m_refreshArea = true;
            }

            x += textwidth;

            if (trailing && background && !last)
            {
                // draw background for trailing space
                QRect space(x, y, trailing, height);
                MythUIShape *shape = new MythUIShape(this,
                        QString("cc708shape%1x%2trail").arg(row).arg(i));
                shape->SetFillBrush(bgfill);
                shape->SetArea(MythRect(space));
                m_708imageCache[num].append(shape);
                m_refreshArea = true;
            }

            x += trailing;
            first = false;
            VERBOSE(VB_VBI, QString("Win %1 row %2 String '%3'")
                .arg(num).arg(row).arg(list[i]->str));
        }
        y += maxheight;
    }
}

void SubtitleScreen::AddScaledImage(QImage &img, QRect &pos)
{
    VideoOutput *vo = m_player->getVideoOutput();
    if (!vo)
        return;

    QRect scaled = vo->GetImageRect(pos);
    if (scaled.size() != pos.size())
    {
        img = img.scaled(scaled.width(), scaled.height(),
                         Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }

    MythPainter *osd_painter = vo->GetOSDPainter();
    MythImage* image = NULL;
    if (osd_painter)
         image = osd_painter->GetFormatImage();

    if (image)
    {
        image->Assign(img);
        MythUIImage *uiimage = new MythUIImage(this, "dvd_button");
        if (uiimage)
        {
            m_refreshArea = true;
            uiimage->SetImage(image);
            uiimage->SetArea(MythRect(scaled));
        }
    }
}

bool SubtitleScreen::InitialiseFont(int fontStretch)
{
    static bool initialised = false;
    QString font = gCoreContext->GetSetting("OSDSubFont", "FreeSans");
    if (initialised)
    {
        if (gTextSubFont->face().family() == font &&
            gTextSubFont->face().stretch() == fontStretch)
            return true;
        delete gTextSubFont;
    }

    MythFontProperties *mythfont = new MythFontProperties();
    if (mythfont)
    {
        QFont newfont(font);
        newfont.setStretch(fontStretch);
        font.detach();
        mythfont->SetFace(newfont);
        mythfont->SetOutline(true, Qt::black, 2, 255);
        gTextSubFont = mythfont;
    }
    else
        return false;

    initialised = true;
    VERBOSE(VB_PLAYBACK, LOC + QString("Loaded main subtitle font '%1'")
        .arg(font));
    return true;
}

bool SubtitleScreen::Initialise708Fonts(int fontStretch)
{
    static bool initialised = false;
    if (initialised)
        return true;

    initialised = true;

    VERBOSE(VB_IMPORTANT, "Initialise708Fonts()");

    // TODO remove extra fonts from settings page
    QStringList fonts;
    fonts.append("Droid Sans Mono"); // default
    fonts.append("FreeMono");        // mono serif
    fonts.append("DejaVu Serif");    // prop serif
    fonts.append("Droid Sans Mono"); // mono sans
    fonts.append("Liberation Sans"); // prop sans
    fonts.append("Purisa");          // casual
    fonts.append("URW Chancery L");  // cursive
    fonts.append("Impact");          // capitals

    int count = 0;
    foreach(QString font, fonts)
    {
        MythFontProperties *mythfont = new MythFontProperties();
        if (mythfont)
        {
            QFont newfont(font);
            newfont.setStretch(fontStretch);
            font.detach();
            mythfont->SetFace(newfont);
            gCC708Fonts.insert(count, mythfont);
            count++;
        }
    }
    VERBOSE(VB_PLAYBACK, LOC + QString("Loaded %1 CEA-708 fonts").arg(count));
    return true;
}

MythFontProperties* SubtitleScreen::Get708Font(CC708CharacterAttribute attr)
{
    MythFontProperties *mythfont = gCC708Fonts[attr.font_tag & 0x7];
    if (!mythfont)
        return NULL;

    mythfont->GetFace()->setItalic(attr.italics);
    mythfont->GetFace()->setPixelSize(m_708fontSizes[attr.pen_size & 0x3]);
    mythfont->GetFace()->setUnderline(attr.underline);
    mythfont->SetColor(attr.GetFGColor());

    int off = m_708fontSizes[attr.pen_size & 0x3] / 20;
    QPoint shadowsz(off, off);
    QColor colour = attr.GetEdgeColor();
    int alpha     = attr.GetFGAlpha();
    bool outline = false;
    bool shadow  = false;

    if (attr.edge_type == k708AttrEdgeLeftDropShadow)
    {
        shadow = true;
        shadowsz.setX(-off);
    }
    else if (attr.edge_type == k708AttrEdgeRightDropShadow)
    {
        shadow = true;
    }
    else if (attr.edge_type == k708AttrEdgeUniform ||
             attr.edge_type == k708AttrEdgeRaised  ||
             attr.edge_type == k708AttrEdgeDepressed)
    {
        outline = true;
    }

    mythfont->SetOutline(outline, colour, off, alpha);
    mythfont->SetShadow(shadow, shadowsz, colour, alpha);

    return mythfont;
}
