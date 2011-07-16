#include "mythlogging.h"

#include "streamingringbuffer.h"

#define LOC QString("StreamRingBuf(%1): ").arg(filename)

StreamingRingBuffer::StreamingRingBuffer(const QString &lfilename)
  : RingBuffer(kRingBuffer_HTTP), m_context(NULL)
{
    startreadahead = false;
    OpenFile(lfilename);
}

StreamingRingBuffer::~StreamingRingBuffer()
{
    if (m_context)
        url_close(m_context);
}

bool StreamingRingBuffer::IsOpen(void) const
{
    return m_context;
}

long long StreamingRingBuffer::GetReadPosition(void) const
{
    return 0;
}

bool StreamingRingBuffer::OpenFile(const QString &lfilename, uint retry_ms)
{
    av_register_all();

    safefilename = lfilename;
    filename = lfilename;
    LOG(VB_GENERAL, LOG_INFO, LOC + QString("Trying %1").arg(filename));

    int res = url_open(&m_context, filename.toAscii(), URL_RDONLY);

    if (res < 0 || !m_context)
    {
        LOG(VB_GENERAL, LOG_ERR, LOC +
            QString("Failed to open stream (error %1)") .arg(res));
        return false;
    }

    return true;
}

long long StreamingRingBuffer::Seek(long long pos, int whence, bool has_lock)
{
    if (!m_context)
        return 0;

    if (url_seek(m_context, pos, whence) < 0)
    {
        ateof = true;
        return 0;
    }
    return pos;
}

int StreamingRingBuffer::safe_read(void *data, uint sz)
{
    if (m_context)
        return url_read_complete(m_context, (unsigned char*)data, sz);
    return 0;
}

long long StreamingRingBuffer::GetRealFileSize(void) const
{
    if (m_context)
        return url_filesize(m_context);
    return -1;
}
