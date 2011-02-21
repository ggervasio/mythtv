// ANSI C headers
#include <cstdio>
#include <cstdlib>
#include <cerrno>

// Unix C headers
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <string.h>

// Qt headers
#include <QString>
#include <QThread>

// MythTV headers
#include "ThreadedFileWriter.h"
#include "compat.h"
#include "mythverbose.h"
#include "mythconfig.h" // gives us HAVE_POSIX_FADVISE

#if HAVE_POSIX_FADVISE < 1
static int posix_fadvise(int, off_t, off_t, int) { return 0; }
#define POSIX_FADV_DONTNEED 0
#  if defined(__linux__)
#    warning "Not using fadvise on platform that supports it."
#  endif
#endif

#define LOC QString("TFW: ")
#define LOC_ERR QString("TFW, Error: ")

const uint ThreadedFileWriter::TFW_DEF_BUF_SIZE   = 2*1024*1024;
const uint ThreadedFileWriter::TFW_MAX_WRITE_SIZE = TFW_DEF_BUF_SIZE / 4;
const uint ThreadedFileWriter::TFW_MIN_WRITE_SIZE = TFW_DEF_BUF_SIZE / 32;

/** \class ThreadedFileWriter
 *  \brief This class supports the writing of recordings to disk.
 *
 *   This class allows us manage the buffering when writing to
 *   disk. We write to the kernel image of the disk using one
 *   thread, and sync the kernel's image of the disk to hardware
 *   using another thread. The goal here so to block as little as
 *   possible when the classes using this class want to add data
 *   to the stream.
 */

/** \fn safe_write(int, const void*, uint, bool &ok)
 *  \brief Writes data to disk
 *
 *   This just uses the Standard C write() to write to disk.
 *   We retry forever on EAGAIN errors, and three times on
 *   any other error.
 *
 *  \param fd   File descriptor
 *  \param data Pointer to data to write
 *  \param sz   Size of data to write in bytes
 */
static uint safe_write(int fd, const void *data, uint sz, bool &ok)
{
    int ret;
    uint tot = 0;
    uint errcnt = 0;

    while (tot < sz)
    {
        ret = write(fd, (char *)data + tot, sz - tot);
        if (ret < 0)
        {
            if (errno == EAGAIN)
            {
                VERBOSE(VB_IMPORTANT, LOC + "safe_write(): Got EAGAIN.");
                continue;
            }
            if (errno == ENOSPC)
            {
                VERBOSE(VB_IMPORTANT, LOC + "safe_write(): Got ENOSPC (No space left on device).");
		errcnt = 10;
		tot = 0;
                break;
            }
            errcnt++;
            VERBOSE(VB_IMPORTANT, LOC_ERR + "safe_write(): File I/O " +
                    QString(" errcnt: %1").arg(errcnt) + ENO + QString(" errno: %1").arg(errno));

            if (errcnt == 3)
                break;
        }
        else
        {
            tot += ret;
        }

        if (tot < sz)
        {
            VERBOSE(VB_IMPORTANT, LOC + "safe_write(): funky usleep");
            usleep(1000);
        }
    }
    ok = (errcnt < 3);
    return tot;
}

/** \fn TFWWriteThread::start()
 *  \brief Thunk that runs ThreadedFileWriter::DiskLoop(void)
 */
void TFWWriteThread::run(void)
{
    if (!m_ptr)
        return;

#ifndef USING_MINGW
    signal(SIGXFSZ, SIG_IGN);
#endif
    m_ptr->DiskLoop();
}

/** \fn TFWSyncThread::boot_syncer(void*)
 *  \brief Thunk that runs ThreadedFileWriter::SyncLoop(void)
 */
void TFWSyncThread::run(void)
{
    if (!m_ptr)
        return;

    m_ptr->SyncLoop();
}

/** \fn ThreadedFileWriter::ThreadedFileWriter(const QString&,int,mode_t)
 *  \brief Creates a threaded file writer.
 */
ThreadedFileWriter::ThreadedFileWriter(const QString &fname,
                                       int pflags, mode_t pmode) :
    // file stuff
    filename(fname),                     flags(pflags),
    mode(pmode),                         fd(-1),
    m_file_sync(0),                      m_file_wpos(0),
    // state
    no_writes(false),                    flush(false),
    write_is_blocked(false),             in_dtor(false),
    ignore_writes(false),                tfw_min_write_size(0),
    // buffer position state
    rpos(0),                             wpos(0),
    written(0),
    // buffer
    buf(NULL),                           tfw_buf_size(0)
{
    filename.detach();
}

/** \fn ThreadedFileWriter::Open(void)
 *  \brief Opens the file we will be writing to.
 *  \return true if we successfully open the file.
 */
bool ThreadedFileWriter::Open(void)
{
    ignore_writes = false;

    if (filename == "-")
        fd = fileno(stdout);
    else
    {
        QByteArray fname = filename.toLocal8Bit();
        fd = open(fname.constData(), flags, mode);
    }

    if (fd < 0)
    {
        VERBOSE(VB_IMPORTANT, LOC_ERR +
                QString("Opening file '%1'.").arg(filename) + ENO);
        return false;
    }
    else
    {
#ifdef USING_MINGW
        _setmode(fd, _O_BINARY);
#endif
        buf = new char[TFW_DEF_BUF_SIZE + 1024];
        memset(buf, 0, TFW_DEF_BUF_SIZE + 64);

        m_file_sync =  m_file_wpos = 0;

        tfw_buf_size = TFW_DEF_BUF_SIZE;
        tfw_min_write_size = TFW_MIN_WRITE_SIZE;

        writer.SetPtr(this);
        writer.start();

        if (!writer.isRunning())
        {
            VERBOSE(VB_IMPORTANT, LOC_ERR +
                    QString("Starting writer thread. "));
            return false;
        }

        syncer.SetPtr(this);
        syncer.start();

        if (!syncer.isRunning())
        {
            VERBOSE(VB_IMPORTANT, LOC_ERR +
                    QString("Starting syncer thread. "));
            return false;
        }

        return true;
    }
}

/** \fn ThreadedFileWriter::~ThreadedFileWriter()
 *  \brief Commits all writes and closes the file.
 */
ThreadedFileWriter::~ThreadedFileWriter()
{
    no_writes = true;

    if (fd >= 0)
    {
        Flush();
        in_dtor = true; /* tells child thread to exit */

        bufferSyncWait.wakeAll();
        syncer.wait();

        bufferHasData.wakeAll();
        writer.wait();
        close(fd);
        fd = -1;
    }

    if (buf)
    {
        delete [] buf;
        buf = NULL;
    }
}

/** \fn ThreadedFileWriter::Write(const void*, uint)
 *  \brief Writes data to the end of the write buffer
 *
 *   NOTE: This blocks while buffer is in use by the write to disk thread.
 *
 *  \param data  pointer to data to write to disk
 *  \param count size of data in bytes
 */
uint ThreadedFileWriter::Write(const void *data, uint count)
{
    if (count == 0)
        return 0;

    if (count > tfw_buf_size)
    {
        VERBOSE(VB_IMPORTANT, LOC +
                QString("WARNING: count(%1), tfw_buf_size(%2)")
                .arg(count).arg(tfw_buf_size));
    }

    uint iobound_cnt = 0;
    uint remaining = count;
    char *wdata = (char *)data;

    while (remaining)
    {
        bool first = true;

        buflock.lock();
        while (BufFreePriv() == 0)
        {
            if (first)
            {
                ++iobound_cnt;
                VERBOSE(VB_IMPORTANT, LOC_ERR + "Write() -- IOBOUND begin " +
                        QString("remaining(%1) free(%2) size(%3) cnt(%4)")
                        .arg(remaining).arg(BufFreePriv())
                        .arg(tfw_buf_size).arg(iobound_cnt));
                first = false;
            }

            bufferWroteData.wait(&buflock, 1000);
        }
        uint twpos = wpos;
        uint bytes = (BufFreePriv() < remaining) ? BufFreePriv() : remaining;
        buflock.unlock();

        if (!first)
            VERBOSE(VB_IMPORTANT, LOC_ERR + "Write() -- IOBOUND end");

        if (no_writes)
            return 0;

        if ((twpos + bytes) > tfw_buf_size)
        {
            int first_chunk_size  = tfw_buf_size - twpos;
            int second_chunk_size = bytes - first_chunk_size;
            memcpy(buf + twpos, wdata, first_chunk_size);
            memcpy(buf, wdata + first_chunk_size,
                   second_chunk_size);
        }
        else
        {
            memcpy(buf + twpos, wdata, bytes);
        }

        buflock.lock();
        if (twpos == wpos)
        {
            wpos = (wpos + bytes) % tfw_buf_size;
        }
        else
        {
            VERBOSE(VB_IMPORTANT, LOC_ERR + "Programmer Error detected! "
                    "wpos was changed from under the Write() function.");
        }
        buflock.unlock();

        bufferHasData.wakeAll();

        remaining -= bytes;
        wdata += bytes;

        if (remaining)
        {
            buflock.lock();
            if (0 == BufFreePriv())
                bufferWroteData.wait(&buflock, 10000);
            buflock.unlock();
        }
    }

    return count;
}

/** \fn ThreadedFileWriter::Seek(long long pos, int whence)
 *  \brief Seek to a position within stream; May be unsafe.
 *
 *   This method is unsafe if Start() has been called and
 *   the call us not preceeded by StopReads(). You probably
 *   want to follow Seek() with a StartReads() in this case.
 *
 *   This method assumes that we don't seek very often. It does
 *   not use a high performance approach... we just block until
 *   the write thread empties the buffer.
 */
long long ThreadedFileWriter::Seek(long long pos, int whence)
{
    Flush();

    return lseek(fd, pos, whence);
}

/** \fn ThreadedFileWriter::Flush(void)
 *  \brief Allow DiskLoop() to flush buffer completely ignoring low watermark.
 */
void ThreadedFileWriter::Flush(void)
{
    QMutexLocker locker(&buflock);
    flush = true;
    while (BufUsedPriv() > 0)
    {
        if (!bufferEmpty.wait(locker.mutex(), 2000))
            VERBOSE(VB_IMPORTANT, LOC + "Taking a long time to flush..");
    }
    flush = false;
}

/** \brief Flush data written to the file descriptor to disk.
 *
 *  This prevents freezing up Linux disk access on a running
 *  CFQ, AS, or Deadline as the disk write schedulers. It does
 *  this via two mechanism. One is a data sync using the best
 *  mechanism available (fdatasync then fsync). The second is
 *  by telling the kernel we do not intend to use the data just
 *  written anytime soon so other processes time-slices will
 *  not be used to deal with our excess dirty pages.
 *
 *  \note We used to also use sync_file_range on Linux, however
 *  this is incompatible with newer filesystems such as BRTFS and
 *  does not actually sync any blocks that have not been allocated
 *  yet so it was never really appropriate for ThreadedFileWriter.
 *
 *  \note We use standard posix calls for this, so any operating
 *  system supporting the calls will benefit, but this has been
 *  designed with Linux in mind. Other OS's may benefit from
 *  revisiting this function.
 */
void ThreadedFileWriter::Sync(void)
{
    if (fd >= 0)
    {
        /// Toss any data the kernel wrote to disk on it's own from
        /// the cache, so we don't get penalized for preserving it
        /// during the sync.
        (void) posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);

#if defined(_POSIX_SYNCHRONIZED_IO) && _POSIX_SYNCHRONIZED_IO > 0
        // fdatasync tries to avoid updating metadata, but will in
        // practice always update metadata if any data is written
        // as the file will usually have grown.
        fdatasync(fd);
#else
        fsync(fd);
#endif

        // Toss any data we just synced from cache, so we don't
        // get penalized for it between now and the next sync.
        (void) posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
    }
}

/** \fn ThreadedFileWriter::SetWriteBufferSize(uint)
 *  \brief Sets the total size of the write buffer.
 *  WARNING: This is not safe when another thread is writing to the buffer.
 */
void ThreadedFileWriter::SetWriteBufferSize(uint newSize)
{
    if (newSize <= 0)
        return;

    Flush();

    QMutexLocker locker(&buflock);
    delete [] buf;
    rpos = wpos = 0;
    buf = new char[newSize + 1024];
    memset(buf, 0, newSize + 64);
    tfw_buf_size = newSize;
}

/** \fn ThreadedFileWriter::SetWriteBufferMinWriteSize(uint)
 *  \brief Sets the minumum number of bytes to write to disk in a single write.
 *         This is ignored during a Flush(void)
 */
void ThreadedFileWriter::SetWriteBufferMinWriteSize(uint newMinSize)
{
    if (newMinSize <= 0)
        return;

    tfw_min_write_size = newMinSize;
}

/** \fn ThreadedFileWriter::SyncLoop(void)
 *  \brief The thread run method that calls Sync(void).
 */
void ThreadedFileWriter::SyncLoop(void)
{
    while (!in_dtor)
    {
        buflock.lock();
        int mstimeout = (written > tfw_min_write_size) ? 1000 : 100;
        bufferSyncWait.wait(&buflock, mstimeout);
        buflock.unlock();

        Sync();
    }
}

/** \fn ThreadedFileWriter::DiskLoop(void)
 *  \brief The thread run method that actually calls safe_write().
 */
void ThreadedFileWriter::DiskLoop(void)
{
    uint size = 0;
    written = 0;

    while (!in_dtor || BufUsed() > 0)
    {
        buflock.lock();
        size = BufUsedPriv();

        if (size == 0)
        {
            buflock.unlock();
            bufferEmpty.wakeAll();
            buflock.lock();
        }

        if (!size || (!in_dtor && !flush &&
            ((size < tfw_min_write_size) &&
             (written >= tfw_min_write_size))))
        {
            bufferHasData.wait(&buflock, 100);
            buflock.unlock();
            continue;
        }
        uint trpos = rpos;
        buflock.unlock();

        /* cap the max. write size. Prevents the situation where 90% of the
           buffer is valid, and we try to write all of it at once which
           takes a long time. During this time, the other thread fills up
           the 10% that was free... */
        size = (size > TFW_MAX_WRITE_SIZE) ? TFW_MAX_WRITE_SIZE : size;

        bool write_ok;
        if (ignore_writes)
            ;
        else if ((trpos + size) > tfw_buf_size)
        {
            int first_chunk_size  = tfw_buf_size - trpos;
            int second_chunk_size = size - first_chunk_size;
            size = safe_write(fd, buf + trpos, first_chunk_size, write_ok);
            if ((int)size == first_chunk_size && write_ok)
                size += safe_write(fd, buf, second_chunk_size, write_ok);
        }
        else
        {
            size = safe_write(fd, buf + trpos, size, write_ok);
        }

        if (!ignore_writes && !write_ok && ((EFBIG == errno) || (ENOSPC == errno)))
        {
            QString msg;
            switch (errno)
            {
                case EFBIG:
                    msg =
                        "Maximum file size exceeded by '%1'"
                        "\n\t\t\t"
                        "You must either change the process ulimits, configure"
                        "\n\t\t\t"
                        "your operating system with \"Large File\" support, or use"
                        "\n\t\t\t"
                        "a filesystem which supports 64-bit or 128-bit files."
                        "\n\t\t\t"
                        "HINT: FAT32 is a 32-bit filesystem.";
                    break;
                case ENOSPC:
                    msg =
                        "No space left on the device for file '%1'"
                        "\n\t\t\t"
                        "file will be truncated, no further writing will be done.";
                    break;
            }

            VERBOSE(VB_IMPORTANT, msg.arg(filename));
            ignore_writes = true;
        }

        if (written <= tfw_min_write_size)
        {
            written += size;
        }

        buflock.lock();
        if (trpos == rpos)
        {
            rpos = (rpos + size) % tfw_buf_size;
        }
        else
        {
            VERBOSE(VB_IMPORTANT, LOC_ERR + "Programmer Error detected! "
                    "rpos was changed from under the DiskLoop() function.");
        }
        m_file_wpos += size;
        buflock.unlock();

        bufferWroteData.wakeAll();
    }
}

/** \fn ThreadedFileWriter::BufUsedPriv(void) const
 *  \brief Number of bytes queued for write by the write thread.
 */
uint ThreadedFileWriter::BufUsedPriv(void) const
{
    return (wpos >= rpos) ? wpos - rpos : tfw_buf_size - rpos + wpos;
}

/** \fn ThreadedFileWriter::BufFreePriv(void) const
 *  \brief Number of bytes that can be written without blocking.
 */
uint ThreadedFileWriter::BufFreePriv(void) const
{
    return ((wpos >= rpos) ? (rpos + tfw_buf_size) : rpos) - wpos - 1;
}

/** \fn ThreadedFileWriter::BufUsed(void) const
 *  \brief Number of bytes queued for write by the write thread. With locking.
 */
uint ThreadedFileWriter::BufUsed(void) const
{
    QMutexLocker locker(&buflock);
    return (wpos >= rpos) ? wpos - rpos : tfw_buf_size - rpos + wpos;
}

/**
 *  \brief Number of bytes that can be written without blocking. With locking.
 */
uint ThreadedFileWriter::BufFree(void) const
{
    QMutexLocker locker(&buflock);
    return ((wpos >= rpos) ? (rpos + tfw_buf_size) : rpos) - wpos - 1;
}

