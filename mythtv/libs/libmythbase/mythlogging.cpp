#include <QMutex>
#include <QMutexLocker>
#include <QList>
#include <QQueue>
#include <QThread>
#include <QHash>
#include <QCoreApplication>

#define _LogLevelNames_
#include "mythlogging.h"
#include "mythverbose.h"
#include "mythconfig.h"
#include "mythdb.h"
#include "mythcorecontext.h"
#include "dbutil.h"

#include <stdlib.h>
#define SYSLOG_NAMES
#include <syslog.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#if HAVE_GETTIMEOFDAY
#include <sys/time.h>
#endif
#include <signal.h>

// Various ways to get to thread's tid
#if defined(linux)
#include <sys/syscall.h>
#elif defined(__FreeBSD__)
#include <sys/ucontext.h>
#include <sys/thr.h>
#elif CONFIG_DARWIN
#include <mach/mach.h>
#endif

QMutex                  loggerListMutex;
QList<LoggerBase *>     loggerList;

QMutex                  logQueueMutex;
QQueue<LoggingItem_t *> logQueue;

QMutex                  logThreadMutex;
QHash<uint64_t, char *> logThreadHash;

QMutex                   logThreadTidMutex;
QHash<uint64_t, int64_t> logThreadTidHash;

LoggerThread            logThread;
bool                    debugRegistration = false;

#define TIMESTAMP_MAX 30
#define MAX_STRING_LENGTH 2048

LogLevel_t LogLevel = LOG_UNKNOWN;  /**< The log level mask to apply, messages
                                         must be at at least this priority to
                                         be output */

char *getThreadName( LoggingItem_t *item );
int64_t getThreadTid( LoggingItem_t *item );
void setThreadTid( LoggingItem_t *item );
void deleteItem( LoggingItem_t *item );
void logSighup( int signum, siginfo_t *info, void *secret );

LoggerBase::LoggerBase(char *string, int number)
{
    QMutexLocker locker(&loggerListMutex);
    if (string)
    {
        m_handle.string = strdup(string);
        m_string = true;
    }
    else
    {
        m_handle.number = number;
        m_string = false;
    }
    loggerList.append(this);
}

LoggerBase::~LoggerBase()
{
    QMutexLocker locker(&loggerListMutex);

    QList<LoggerBase *>::iterator it;

    for(it = loggerList.begin(); it != loggerList.end(); it++)
    {
        if( *it == this )
        {
            loggerList.erase(it);
            break;
        }
    }

    if (m_string)
        free(m_handle.string);
}


FileLogger::FileLogger(char *filename) : LoggerBase(filename, 0),
                                         m_opened(false), m_fd(-1)
{
    if( !strcmp(filename, "-") )
    {
        m_opened = true;
        m_fd = 1;
        LogPrint( VB_IMPORTANT, LOG_INFO, "Added logging to the console" );
    }
    else
    {
        m_fd = open(filename, O_WRONLY|O_CREAT|O_APPEND, 0664);
        m_opened = (m_fd != -1);
        LogPrint( VB_IMPORTANT, LOG_INFO, "Added logging to %s", filename );
    }
}

FileLogger::~FileLogger()
{
    if( m_opened )
    {
        if( m_fd != 1 )
        {
            LogPrint( VB_IMPORTANT, LOG_INFO, "Removed logging to %s",
                      m_handle.string );
            close( m_fd );
        }
        else
            LogPrint( VB_IMPORTANT, LOG_INFO,
                      "Removed logging to the console" );
    }
}

void FileLogger::reopen(void)
{
    char *filename = m_handle.string;

    // Skip console
    if( !strcmp(filename, "-") )
        return;

    close(m_fd);

    m_fd = open(filename, O_WRONLY|O_CREAT|O_APPEND, 0664);
    m_opened = (m_fd != -1);
    LogPrint( VB_IMPORTANT, LOG_INFO, "Rolled logging on %s", filename );
}

bool FileLogger::logmsg(LoggingItem_t *item)
{
    char                line[MAX_STRING_LENGTH];
    char                usPart[9];
    char                timestamp[TIMESTAMP_MAX];
    int                 length;
    char               *threadName = NULL;
    pid_t               pid = getpid();
    pid_t               tid = 0;

    if (!m_opened)
        return false;

    strftime( timestamp, TIMESTAMP_MAX-8, "%Y-%m-%d %H:%M:%S",
              (const struct tm *)&item->tm );
    snprintf( usPart, 9, ".%06d", (int)(item->usec) );
    strcat( timestamp, usPart );
    length = strlen( timestamp );

    if (m_fd == 1)
    {
        // Stdout
        snprintf( line, MAX_STRING_LENGTH, "%s %c  %s\n", timestamp,
                  LogLevelShortNames[item->level], item->message );
    }
    else
    {
        threadName = getThreadName(item);
        tid = getThreadTid(item);

        if( tid )
            snprintf( line, MAX_STRING_LENGTH, 
                      "%s %c [%d/%d] %s %s:%d (%s) - %s\n",
                      timestamp, LogLevelShortNames[item->level], pid, tid,
                      threadName, item->file, item->line, item->function,
                      item->message );
        else
            snprintf( line, MAX_STRING_LENGTH,
                      "%s %c [%d] %s %s:%d (%s) - %s\n",
                      timestamp, LogLevelShortNames[item->level], pid,
                      threadName, item->file, item->line, item->function,
                      item->message );
    }

    int result = write( m_fd, line, strlen(line) );

    {
        QMutexLocker locker((QMutex *)item->refmutex);
        item->refcount--;
    }

    if( result == -1 )
    {
        LogPrint( VB_IMPORTANT, LOG_UNKNOWN,
                  "Closed Log output on fd %d due to errors", m_fd );
        m_opened = false;
        if( m_fd != 1 )
            close( m_fd );
        return false;
    }
    return true;
}


SyslogLogger::SyslogLogger(int facility) : LoggerBase(NULL, facility),
                                           m_opened(false)
{
    CODE *name;

    m_application = strdup((char *)QCoreApplication::applicationName()
                           .toLocal8Bit().constData());

    openlog( m_application, LOG_NDELAY | LOG_PID, facility );
    m_opened = true;

    for( name = &facilitynames[0];
         name->c_name && name->c_val != facility; name++ );

    LogPrint(VB_IMPORTANT, LOG_INFO, "Added syslogging to facility %s",
             name->c_name);
}

SyslogLogger::~SyslogLogger()
{
    LogPrint(VB_IMPORTANT, LOG_INFO, "Removing syslogging");
    free(m_application);
    closelog();
}

bool SyslogLogger::logmsg(LoggingItem_t *item)
{
    if (!m_opened)
        return false;

    syslog( item->level, "%s", item->message );

    {
        QMutexLocker locker((QMutex *)item->refmutex);
        item->refcount--;
    }

    return true;
}


DatabaseLogger::DatabaseLogger(char *table) : LoggerBase(table, 0),
                                              m_host(NULL), m_opened(false),
                                              m_loggingTableExists(false)
{
    static const char *queryFmt =
        "INSERT INTO %s (host, application, pid, thread, "
        "msgtime, level, message) VALUES (:HOST, :APPLICATION, "
        ":PID, :THREAD, :MSGTIME, :LEVEL, :MESSAGE)";

    LogPrint(VB_IMPORTANT, LOG_INFO, "Added database logging to table %s",
             m_handle.string);

    if (gCoreContext && !gCoreContext->GetHostName().isEmpty())
        m_host = strdup((char *)gCoreContext->GetHostName()
                        .toLocal8Bit().constData());

    m_application = strdup((char *)QCoreApplication::applicationName()
                           .toLocal8Bit().constData());
    m_pid = getpid();

    m_query = (char *)malloc(strlen(queryFmt) + strlen(m_handle.string));
    sprintf(m_query, queryFmt, m_handle.string);

    m_thread = new DBLoggerThread(this);
    m_thread->start();

    m_opened = true;
}

DatabaseLogger::~DatabaseLogger()
{
    LogPrint(VB_IMPORTANT, LOG_INFO, "Removing database logging");

    if( m_thread )
    {
        m_thread->stop();
        m_thread->wait();
        delete m_thread;
    }

    if( m_query )
        free(m_query);
    if( m_application )
        free(m_application);
    if( m_host )
        free(m_host);
}

bool DatabaseLogger::logmsg(LoggingItem_t *item)
{
    if( m_thread )
        m_thread->enqueue(item);
    return true;
}

bool DatabaseLogger::logqmsg(LoggingItem_t *item)
{
    char        timestamp[TIMESTAMP_MAX];
    char       *threadName = getThreadName(item);;

    if( !isDatabaseReady() )
        return false;

    strftime( timestamp, TIMESTAMP_MAX-8, "%Y-%m-%d %H:%M:%S",
              (const struct tm *)&item->tm );

    if( gCoreContext && !m_host )
        m_host = strdup((char *)gCoreContext->GetHostName()
                        .toLocal8Bit().constData());

    MSqlQuery   query(MSqlQuery::InitCon());
    query.prepare( m_query );
    query.bindValue(":HOST",        m_host);
    query.bindValue(":APPLICATION", m_application);
    query.bindValue(":PID",         m_pid);
    query.bindValue(":THREAD",      threadName);
    query.bindValue(":MSGTIME",     timestamp);
    query.bindValue(":LEVEL",       item->level);
    query.bindValue(":MESSAGE",     item->message);

    {
        QMutexLocker locker((QMutex *)item->refmutex);
        item->refcount--;
    }

    if (!query.exec())
    {
        MythDB::DBError("DBLogging", query);
        return false;
    }

    return true;
}

void DBLoggerThread::run(void)
{
    threadRegister("DBLogger");
    LoggingItem_t *item;

    aborted = false;

    QMutexLocker qLock(&m_queueMutex);

    while(!aborted || !m_queue->isEmpty())
    {
        if (m_queue->isEmpty())
        {
            qLock.unlock();
            msleep(100);
            qLock.relock();
            continue;
        }

        item = m_queue->dequeue();
        if (!item)
            continue;

        qLock.unlock();

        if( item->message && !aborted )
        {
            m_logger->logqmsg(item);
        }

        deleteItem(item);

        qLock.relock();
    }
}

bool DatabaseLogger::isDatabaseReady()
{
    bool ready = false;
    MythDB *db;

    if ( !m_loggingTableExists )
        m_loggingTableExists = DBUtil::TableExists(m_handle.string);

    if ( m_loggingTableExists && (db = GetMythDB()) && db->HaveValidDatabase() )
        ready = true;

    return ready;
}

char *getThreadName( LoggingItem_t *item )
{
    static const char  *unknown = "thread_unknown";
    char *threadName;

    if( !item )
        return( (char *)unknown );

    if( !item->threadName )
    {
        QMutexLocker locker(&logThreadMutex);
        if( logThreadHash.contains(item->threadId) )
            threadName = logThreadHash[item->threadId];
        else
            threadName = (char *)unknown;
    }
    else
    {
        threadName = item->threadName;
    }

    return( threadName );
}

int64_t getThreadTid( LoggingItem_t *item )
{
    pid_t tid = 0;

    if( !item )
        return( 0 );

    QMutexLocker locker(&logThreadTidMutex);
    if( logThreadTidHash.contains(item->threadId) )
        tid = logThreadTidHash[item->threadId];

    return( tid );
}

void setThreadTid( LoggingItem_t *item )
{
    int64_t tid = 0;

    QMutexLocker locker(&logThreadTidMutex);

    if( ! logThreadTidHash.contains(item->threadId) )
    {
#if defined(linux)
        tid = (int64_t)syscall(SYS_gettid);
#elif defined(__FreeBSD__) && 0
        long lwpid;
        int dummy = thr_self( &lwpid );
        tid = (int64_t)lwpid;
#elif CONFIG_DARWIN
        tid = (int64_t)mach_thread_self();
#endif
        logThreadTidHash[item->threadId] = tid;
    }
}


LoggerThread::LoggerThread()
{
    char *debug = getenv("VERBOSE_THREADS");
    if (debug != NULL)
    {
        VERBOSE(VB_IMPORTANT, "Logging thread registration/deregistration "
                              "enabled!");
        debugRegistration = true;
    }
}

LoggerThread::~LoggerThread()
{
    QMutexLocker locker(&loggerListMutex);

    QList<LoggerBase *>::iterator it;

    for(it = loggerList.begin(); it != loggerList.end(); it++)
    {
        (*it)->deleteLater();
    }
}

void LoggerThread::run(void)
{
    threadRegister("Logger");
    LoggingItem_t *item;

    aborted = false;

    QMutexLocker qLock(&logQueueMutex);

    while(!aborted || !logQueue.isEmpty())
    {
        if (logQueue.isEmpty())
        {
            qLock.unlock();
            msleep(100);
            qLock.relock();
            continue;
        }

        item = logQueue.dequeue();
        if (!item)
            continue;

        qLock.unlock();

        if (item->registering)
        {
            int64_t tid = getThreadTid(item);

            QMutexLocker locker(&logThreadMutex);
            logThreadHash[item->threadId] = strdup(item->threadName);

            if( debugRegistration )
            {
                item->message   = (char *)malloc(LOGLINE_MAX+1);
                if( item->message )
                {
                    snprintf( item->message, LOGLINE_MAX,
                              "Thread 0x%llX (%lld) registered as \'%s\'",
                              (long long unsigned int)item->threadId,
                              tid, logThreadHash[item->threadId] );
                }
            }
        }
        else if (item->deregistering)
        {
            int64_t tid = 0;

            {
                QMutexLocker locker(&logThreadTidMutex);
                if( logThreadTidHash.contains(item->threadId) )
                {
                    tid = logThreadTidHash[item->threadId];
                    logThreadTidHash.remove(item->threadId);
                }
            }

            QMutexLocker locker(&logThreadMutex);
            if( logThreadHash.contains(item->threadId) )
            {
                if( debugRegistration )
                {
                    item->message   = (char *)malloc(LOGLINE_MAX+1);
                    if( item->message )
                    {
                        snprintf( item->message, LOGLINE_MAX,
                                  "Thread 0x%llX (%lld) deregistered as \'%s\'",
                                  (long long unsigned int)item->threadId,
                                  tid, logThreadHash[item->threadId] );
                    }
                }
                item->threadName = logThreadHash[item->threadId];
                logThreadHash.remove(item->threadId);
            }
        }

        if( item->message )
        {
            QMutexLocker locker(&loggerListMutex);

            QList<LoggerBase *>::iterator it;

            item->refcount = loggerList.size();
            item->refmutex = new QMutex;

            for(it = loggerList.begin(); it != loggerList.end(); it++)
            {
                (*it)->logmsg(item);
            }
        }

        deleteItem(item);

        qLock.relock();
    }
}

void deleteItem( LoggingItem_t *item )
{
    if( !item )
        return;

    {
        QMutexLocker locker((QMutex *)item->refmutex);
        if( item->refcount != 0 )
            return;

        if( item->message )
            free(item->message);

        if( item->threadName )
            free( item->threadName );
    }

    delete (QMutex *)item->refmutex;
    delete item;
}

void LogTimeStamp( time_t *epoch, uint32_t *usec )
{
    if( !epoch || !usec )
        return;

#if HAVE_GETTIMEOFDAY
    struct timeval  tv;
    gettimeofday(&tv, NULL);
    *epoch = tv.tv_sec;
    *usec  = tv.tv_usec;
#else
    /* Stupid system has no gettimeofday, use less precise QDateTime */
    QDateTime date = QDateTime::currentDateTime();
    QTime     time = date.time();
    *epoch = date.toTime_t();
    *usec = time.msec() * 1000;
#endif
}

void LogPrintLine( uint32_t mask, LogLevel_t level, const char *file, int line,
                   const char *function, const char *format, ... )
{
    va_list         arguments;
    char           *message;
    LoggingItem_t  *item;
    time_t          epoch;
    uint32_t        usec;

    if( !VERBOSE_LEVEL_CHECK(mask) )
        return;

    if( level > LogLevel )
        return;

    item = new LoggingItem_t;
    if (!item)
        return;

    memset( item, 0, sizeof(LoggingItem_t) );

    message = (char *)malloc(LOGLINE_MAX+1);
    if( !message )
        return;

    va_start(arguments, format);
    vsnprintf(message, LOGLINE_MAX, format, arguments);
    va_end(arguments);

    LogTimeStamp( &epoch, &usec );

    localtime_r(&epoch, &item->tm);
    item->usec     = usec;

    item->level    = level;
    item->file     = file;
    item->line     = line;
    item->function = function;
    item->threadId = (uint64_t)QThread::currentThreadId();
    item->message  = message;
    setThreadTid(item);

    QMutexLocker qLock(&logQueueMutex);
    logQueue.enqueue(item);
}

void logSighup( int signum, siginfo_t *info, void *secret )
{
    VERBOSE(VB_GENERAL, "SIGHUP received, rolling log files.");

    /* SIGHUP was sent.  Close and reopen debug logfiles */
    QMutexLocker locker(&loggerListMutex);

    QList<LoggerBase *>::iterator it;
    for(it = loggerList.begin(); it != loggerList.end(); it++)
    {
        (*it)->reopen();
    }
}


void logStart(QString logfile, int quiet, int facility, bool dblog)
{
    LoggerBase *logger;
    struct sigaction sa;

    if (logThread.isRunning())
        return;

    /* log to the console */
    if( !quiet )
        logger = new FileLogger((char *)"-");

    /* Debug logfile */
    if( !logfile.isEmpty() )
        logger = new FileLogger((char *)logfile.toLocal8Bit().constData());

    /* Syslog */
    if( facility < 0 )
        LogPrint(VB_IMPORTANT, LOG_CRIT,
                 "Syslogging facility unknown, disabling syslog output");
    else if( facility > 0 )
        logger = new SyslogLogger(facility);

    /* Database */
    if( dblog )
        logger = new DatabaseLogger((char *)"logging");

    /* Setup SIGHUP */
    LogPrint(VB_IMPORTANT, LOG_CRIT, "Setting up SIGHUP handler");
    sa.sa_sigaction = logSighup;
    sigemptyset( &sa.sa_mask );
    sa.sa_flags = SA_RESTART | SA_SIGINFO;
    sigaction( SIGHUP, &sa, NULL );

    logThread.start();
}

void logStop(void)
{
    struct sigaction sa;

    logThread.stop();
    logThread.wait();

    /* Tear down SIGHUP */
    sa.sa_handler = SIG_DFL;
    sigemptyset( &sa.sa_mask );
    sa.sa_flags = SA_RESTART;
    sigaction( SIGHUP, &sa, NULL );

    QMutexLocker locker(&loggerListMutex);
    QList<LoggerBase *>::iterator it;

    for(it = loggerList.begin(); it != loggerList.end(); )
    {
        locker.unlock();
        delete *it;
        locker.relock();
        it = loggerList.begin();
    }
}

void threadRegister(QString name)
{
    uint64_t id = (uint64_t)QThread::currentThreadId();
    LoggingItem_t  *item;
    time_t epoch;
    uint32_t usec;

    item = new LoggingItem_t;
    if (!item)
        return;

    memset( item, 0, sizeof(LoggingItem_t) );
    LogTimeStamp( &epoch, &usec );

    localtime_r(&epoch, &item->tm);
    item->usec = usec;

    item->level = (LogLevel_t)LOG_DEBUG;
    item->threadId = id;
    item->line = __LINE__;
    item->file = (char *)__FILE__;
    item->function = (char *)__FUNCTION__;
    item->threadName = strdup((char *)name.toLocal8Bit().constData());
    item->registering = true;
    setThreadTid(item);

    QMutexLocker qLock(&logQueueMutex);
    logQueue.enqueue(item);
}

void threadDeregister(void)
{
    uint64_t id = (uint64_t)QThread::currentThreadId();
    LoggingItem_t  *item;
    time_t epoch;
    uint32_t usec;

    item = new LoggingItem_t;
    if (!item)
        return;

    memset( item, 0, sizeof(LoggingItem_t) );
    LogTimeStamp( &epoch, &usec );

    localtime_r(&epoch, &item->tm);
    item->usec = usec;

    item->level = (LogLevel_t)LOG_DEBUG;
    item->threadId = id;
    item->line = __LINE__;
    item->file = (char *)__FILE__;
    item->function = (char *)__FUNCTION__;
    item->deregistering = true;

    QMutexLocker qLock(&logQueueMutex);
    logQueue.enqueue(item);
}

int syslogGetFacility(QString facility)
{
    CODE *name;
    int i;
    char *string = (char *)facility.toLocal8Bit().constData();

    for( i = 0, name = &facilitynames[0];
         name->c_name && strcmp(name->c_name, string); i++, name++ );

    return( name->c_val );
}

/*
 * vim:ts=4:sw=4:ai:et:si:sts=4
 */
