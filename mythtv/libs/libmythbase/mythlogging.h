#ifndef MYTHLOGGING_H_
#define MYTHLOGGING_H_

#ifdef __cplusplus
#include <QString>
#include <QThread>
#include <QQueue>
#include <QMutex>
#include <QMutexLocker>
#endif
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>

#include "mythbaseexp.h"  //  MBASE_PUBLIC , etc.
#include "verbosedefs.h"

#define LOGLINE_MAX 2048

typedef enum
{
    LOG_EMERG = 0,
    LOG_ALERT,
    LOG_CRIT,
    LOG_ERR,
    LOG_WARNING,
    LOG_NOTICE,
    LOG_INFO,
    LOG_DEBUG,
    LOG_UNKNOWN
} LogLevel_t;

#ifdef _LogLevelNames_
const char *LogLevelNames[] =
{
    "LOG_EMERG",
    "LOG_ALERT",
    "LOG_CRIT",
    "LOG_ERR",
    "LOG_WARNING",
    "LOG_NOTICE",
    "LOG_INFO",
    "LOG_DEBUG",
    "LOG_UNKNOWN"
};
int LogLevelNameCount = sizeof(LogLevelNames) / sizeof(LogLevelNames[0]);

const char LogLevelShortNames[] =
{
    '!',
    'A',
    'C',
    'E',
    'W',
    'N',
    'I',
    'D',
    '-'
};
int LogLevelShortNameCount = sizeof(LogLevelShortNames) / 
                             sizeof(LogLevelShortNames[0]);
#else
extern MBASE_PUBLIC char *LogLevelNames[];
extern MBASE_PUBLIC int LogLevelNameCount;
extern MBASE_PUBLIC char *LogLevelShortNames[];
extern MBASE_PUBLIC int LogLevelShortNameCount;
#endif
extern MBASE_PUBLIC LogLevel_t LogLevel;

typedef struct
{
    LogLevel_t          level;
    uint64_t            threadId;
    const char         *file;
    int                 line;
    const char         *function;
    struct tm           tm;
    uint32_t            usec;
    char               *message;
    char               *threadName;
    int                 registering;
    int                 deregistering;
    int                 refcount;
    void               *refmutex;
} LoggingItem_t;


#ifdef __cplusplus
extern "C" {
#endif

#ifdef __cplusplus
#define LogPrint(mask, level, string) \
    LogPrintLine(mask, (LogLevel_t)level, __FILE__, __LINE__, __FUNCTION__, \
                 QString(string).toLocal8Bit().constData())
#else
#define LogPrint(mask, level, format, ...) \
    LogPrintLine(mask, (LogLevel_t)level, __FILE__, __LINE__, __FUNCTION__, \
                 (const char *)format, ##__VA_ARGS__)
#endif

/* Define the external prototype */
MBASE_PUBLIC void LogPrintLine( uint32_t mask, LogLevel_t level, 
                                const char *file, int line, 
                                const char *function, const char *format, ... );

#ifdef __cplusplus
}

extern MBASE_PUBLIC QString logPropagateArgs;

MBASE_PUBLIC void logStart(QString logfile, int quiet = 0, int facility = 0,
                           LogLevel_t level = LOG_INFO, bool dblog = true, 
                           bool propagate = false);
MBASE_PUBLIC void logStop(void);
MBASE_PUBLIC void logPropagateCalc(void);
MBASE_PUBLIC bool logPropagateQuiet(void);

MBASE_PUBLIC void threadRegister(QString name);
MBASE_PUBLIC void threadDeregister(void);
MBASE_PUBLIC int  syslogGetFacility(QString facility);
MBASE_PUBLIC LogLevel_t logLevelGet(QString level);

void LogTimeStamp( time_t *epoch, uint32_t *usec );

typedef union {
    char   *string;
    int     number;
} LoggerHandle_t;


class LoggerBase : public QObject {
    Q_OBJECT

    public:
        LoggerBase(char *string, int number);
        ~LoggerBase();
        virtual bool logmsg(LoggingItem_t *item) = 0;
        virtual void reopen(void) = 0;
    protected:
        LoggerHandle_t m_handle;
        bool m_string;
};

class FileLogger : public LoggerBase {
    public:
        FileLogger(char *filename);
        ~FileLogger();
        bool logmsg(LoggingItem_t *item);
        void reopen(void);
    private:
        bool m_opened;
        int  m_fd;
};

class SyslogLogger : public LoggerBase {
    public:
        SyslogLogger(int facility);
        ~SyslogLogger();
        bool logmsg(LoggingItem_t *item);
        void reopen(void) { };
    private:
        char *m_application;
        bool m_opened;
};

class DBLoggerThread;

class DatabaseLogger : public LoggerBase {
    friend class DBLoggerThread;
    public:
        DatabaseLogger(char *table);
        ~DatabaseLogger();
        bool logmsg(LoggingItem_t *item);
        void reopen(void) { };
    protected:
        bool logqmsg(LoggingItem_t *item);
    private:
        bool isDatabaseReady();
        bool tableExists(const QString &table);

        DBLoggerThread *m_thread;
        char *m_host;
        char *m_application;
        char *m_query;
        pid_t m_pid;
        bool m_opened;
        bool m_loggingTableExists;
};

class LoggerThread : public QThread {
    Q_OBJECT

    public:
        LoggerThread();
        ~LoggerThread();
        void run(void);
        void stop(void) { aborted = true; };
    private:
        bool aborted;
};

class DBLoggerThread : public QThread {
    Q_OBJECT

    public:
        DBLoggerThread(DatabaseLogger *logger) : m_logger(logger), 
            m_queue(new QQueue<LoggingItem_t *>) {}
        ~DBLoggerThread() { delete m_queue; }
        void run(void);
        void stop(void) { aborted = true; }
        bool enqueue(LoggingItem_t *item) 
        { 
            QMutexLocker qLock(&m_queueMutex); 
            m_queue->enqueue(item); 
            return true; 
        }
    private:
        DatabaseLogger *m_logger;
        QMutex m_queueMutex;
        QQueue<LoggingItem_t *> *m_queue;
        bool aborted;
};
#endif

/// This global variable is set at startup with the flags
/// of the verbose messages we want to see.
extern MBASE_PUBLIC uint64_t verboseMask;

// Helper for checking verbose flags outside of VERBOSE macro
#define VERBOSE_LEVEL_NONE        (verboseMask == 0)
#define VERBOSE_LEVEL_CHECK(mask) ((verboseMask & (mask)) == (mask))

// There are two VERBOSE macros now.  One for use with Qt/C++, one for use
// without Qt.
//
// Neither of them will lock the calling thread, but rather put the log message
// onto a queue.

#ifdef __cplusplus
#define VERBOSE(mask, ...) \
    LogPrint((uint64_t)(mask), LOG_INFO, QString(__VA_ARGS__))
#else
#define VERBOSE(mask, ...) \
    LogPrint((uint64_t)(mask), LOG_INFO, __VA_ARGS__)
#endif


#ifdef  __cplusplus
/// Verbose helper function for ENO macro
extern MBASE_PUBLIC QString logStrerror(int errnum);

extern MBASE_PUBLIC QString verboseString;

MBASE_PUBLIC int verboseArgParse(QString arg);

/// This can be appended to the VERBOSE args with 
/// "+".  Please do not use "<<".  It uses
/// a thread safe version of strerror to produce the
/// string representation of errno and puts it on the
/// next line in the verbose output.
#define ENO QString("\n\t\t\teno: ") + logStrerror(errno)
#endif // __cplusplus

#endif

/*
 * vim:ts=4:sw=4:ai:et:si:sts=4
 */
