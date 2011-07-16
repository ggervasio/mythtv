// ANSI C
#include <cstdlib>

// Qt
#include <QVector>
#include <QSqlDriver>
#include <QSemaphore>
#include <QSqlError>
#include <QSqlField>
#include <QSqlRecord>

// MythTV
#include "compat.h"
#include "mythdbcon.h"
#include "mythdb.h"
#include "mythlogging.h"
#include "mythsystem.h"
#include "exitcodes.h"
#include <unistd.h>

static const uint kPurgeTimeout = 60 * 60;

bool TestDatabase(QString dbHostName,
                  QString dbUserName,
                  QString dbPassword,
                  QString dbName,
                  int dbPort)
{
    bool ret = false;

    if (dbHostName.isEmpty() || dbUserName.isEmpty())
        return ret;

    MSqlDatabase *db = new MSqlDatabase("dbtest");

    if (!db)
        return ret;

    DatabaseParams dbparms;
    dbparms.dbName = dbName;
    dbparms.dbUserName = dbUserName;
    dbparms.dbPassword = dbPassword;
    dbparms.dbHostName = dbHostName;
    dbparms.dbPort = dbPort;

    // Just use some sane defaults for these values
    dbparms.wolEnabled = false;
    dbparms.wolReconnect = 1;
    dbparms.wolRetry = 3;
    dbparms.wolCommand = QString();

    db->SetDBParams(dbparms);

    ret = db->OpenDatabase(true);

    delete db;
    db = NULL;

    return ret;
}

MSqlDatabase::MSqlDatabase(const QString &name)
{
    m_name = name;
    m_name.detach();
    m_db = QSqlDatabase::addDatabase("QMYSQL", m_name);
    LOG(VB_GENERAL, LOG_INFO, "Database connection created: " + m_name);

    if (!m_db.isValid())
    {
        LOG(VB_GENERAL, LOG_ERR, "Unable to init db connection.");
        return;
    }
    m_lastDBKick = QDateTime::currentDateTime().addSecs(-60);
}

MSqlDatabase::~MSqlDatabase()
{
    if (m_db.isOpen())
    {
        m_db.close();
        m_db = QSqlDatabase();  // forces a destroy and must be done before
                                // removeDatabase() so that connections
                                // and queries are cleaned up correctly
        QSqlDatabase::removeDatabase(m_name);
        LOG(VB_GENERAL, LOG_INFO, "Database connection deleted: " + m_name);
    }
}

bool MSqlDatabase::isOpen()
{
    if (m_db.isValid())
    {
        if (m_db.isOpen())
            return true;
    }
    return false;
}

bool MSqlDatabase::OpenDatabase(bool skipdb)
{
    if (!m_db.isValid())
    {
        LOG(VB_GENERAL, LOG_ERR,
              "MSqlDatabase::OpenDatabase(), db object is not valid!");
        return false;
    }

    bool connected = true;

    if (!m_db.isOpen())
    {
        if (!skipdb)
            m_dbparms = GetMythDB()->GetDatabaseParams();
        m_db.setDatabaseName(m_dbparms.dbName);
        m_db.setUserName(m_dbparms.dbUserName);
        m_db.setPassword(m_dbparms.dbPassword);
        m_db.setHostName(m_dbparms.dbHostName);

        if (m_dbparms.dbHostName.isEmpty())  // Bootstrapping without a database?
        {
            connected = true;              // Pretend to be connected
            return true;                   // to reduce errors
        }

        if (m_dbparms.dbPort)
            m_db.setPort(m_dbparms.dbPort);

        // Prefer using the faster localhost connection if using standard
        // ports, even if the user specified a DBHostName of 127.0.0.1.  This
        // will cause MySQL to use a Unix socket (on *nix) or shared memory (on
        // Windows) connection.
        if ((m_dbparms.dbPort == 0 || m_dbparms.dbPort == 3306) &&
            m_dbparms.dbHostName == "127.0.0.1")
            m_db.setHostName("localhost");

        connected = m_db.open();

        if (!connected && m_dbparms.wolEnabled)
        {
            int trycount = 0;

            while (!connected && trycount++ < m_dbparms.wolRetry)
            {
                LOG(VB_GENERAL, LOG_INFO,
                    QString("Using WOL to wakeup database server (Try %1 of "
                            "%2)")
                         .arg(trycount).arg(m_dbparms.wolRetry));

                if (myth_system(m_dbparms.wolCommand) != GENERIC_EXIT_OK)
                {
                    LOG(VB_GENERAL, LOG_ERR,
                            QString("Failed to run WOL command '%1'")
                            .arg(m_dbparms.wolCommand));
                }

                sleep(m_dbparms.wolReconnect);
                connected = m_db.open();
            }

            if (!connected)
            {
                LOG(VB_GENERAL, LOG_ERR,
                    "WOL failed, unable to connect to database!");
            }
        }
        if (connected)
        {
            LOG(VB_GENERAL, LOG_INFO,
                    QString("Connected to database '%1' at host: %2")
                            .arg(m_db.databaseName()).arg(m_db.hostName()));

            // WriteDelayed depends on SetHaveDBConnection() and SetHaveSchema()
            // both being called with true, so order is important here.
            GetMythDB()->SetHaveDBConnection(true);
            if (!GetMythDB()->HaveSchema())
            {
                // We can't just check the count of QSqlDatabase::tables()
                // because it returns all tables visible to the user in *all*
                // databases (not just the current DB).
                bool have_schema = false;
                QString sql = "SELECT COUNT( "
                              "         INFORMATION_SCHEMA.TABLES.TABLE_NAME "
                              "       ) "
                              "  FROM INFORMATION_SCHEMA.TABLES "
                              " WHERE INFORMATION_SCHEMA.TABLES.TABLE_SCHEMA "
                              "       = DATABASE() "
                              "   AND INFORMATION_SCHEMA.TABLES.TABLE_TYPE = "
                              "       'BASE TABLE';";
                // We can't use MSqlQuery to determine if we have a schema,
                // since it will open a new connection, which will try to check
                // if we have a schema
                QSqlQuery query = m_db.exec(sql); // don't convert to MSqlQuery
                if (query.next())
                    have_schema = query.value(0).toInt() > 1;
                GetMythDB()->SetHaveSchema(have_schema);
            }
            GetMythDB()->WriteDelayedSettings();
        }
    }

    if (!connected)
    {
        GetMythDB()->SetHaveDBConnection(false);
        LOG(VB_GENERAL, LOG_ERR, "Unable to connect to database!");
        LOG(VB_GENERAL, LOG_ERR, MythDB::DBErrorMessage(m_db.lastError()));
    }

    return connected;
}

bool MSqlDatabase::KickDatabase()
{
    // Some explanation is called for.  This exists because the mysql
    // driver does not gracefully handle the situation where a TCP
    // socketconnection is dropped (for example due to a timeout).  If
    // a Unix domain socket connection is lost, the driver
    // transparently reestablishes the connection and we don't even
    // notice.  However, when this happens with a TCP connection, the
    // driver returns an error for the next query to be executed, and
    // THEN reestablishes the connection (so the second query succeeds
    // with no intervention).
    // mdz, 2003/08/11


    if (m_lastDBKick.secsTo(QDateTime::currentDateTime()) < 30 &&
        m_db.isOpen())
    {
        return true;
    }

    QString query("SELECT NULL;");
    for (unsigned int i = 0 ; i < 2 ; ++i, usleep(50000))
    {
        if (m_db.isOpen())
        {
            QSqlQuery result = m_db.exec(query); // don't convert to MSqlQuery
            if (result.isActive())
            {
                m_lastDBKick = QDateTime::currentDateTime();
                return true;
            }
        }

        if (i == 0)
        {
            m_db.close();
            m_db.open();
        }
        else
            LOG(VB_GENERAL, LOG_ERR, MythDB::DBErrorMessage(m_db.lastError()));
    }

    m_lastDBKick = QDateTime::currentDateTime().addSecs(-60);

    return false;
}

bool MSqlDatabase::Reconnect()
{
    m_db.close();
    m_db.open();

    bool open = m_db.isOpen();
    if (open)
        LOG(VB_GENERAL, LOG_INFO, "MySQL reconnected successfully");

    return open;
}

// -----------------------------------------------------------------------



MDBManager::MDBManager()
{
    m_nextConnID = 0;
    m_connCount = 0;

    m_sem = new QSemaphore(20);

    m_schedCon = NULL;
    m_DDCon = NULL;
    m_LogCon = NULL;
}

MDBManager::~MDBManager()
{
    while (!m_pool.isEmpty())
        delete m_pool.takeFirst();
    delete m_sem;

    closeStaticCon(&m_schedCon);
    closeStaticCon(&m_DDCon);
    closeStaticCon(&m_LogCon);
}

MSqlDatabase *MDBManager::popConnection()
{
    PurgeIdleConnections();

    m_sem->acquire();
    m_lock.lock();

    MSqlDatabase *db;

    if (m_pool.isEmpty())
    {
        db = new MSqlDatabase("DBManager" + QString::number(m_nextConnID++));
        ++m_connCount;
        LOG(VB_GENERAL, LOG_INFO,
                QString("New DB connection, total: %1").arg(m_connCount));
    }
    else
        db = m_pool.takeLast();

    m_lock.unlock();

    db->OpenDatabase();

    return db;
}

void MDBManager::pushConnection(MSqlDatabase *db)
{
    m_lock.lock();

    if (db)
    {
        db->m_lastDBKick = QDateTime::currentDateTime();
        m_pool.prepend(db);
    }

    m_lock.unlock();
    m_sem->release();

    PurgeIdleConnections();
}

void MDBManager::PurgeIdleConnections(void)
{
    QMutexLocker locker(&m_lock);

    QDateTime now = QDateTime::currentDateTime();
    QList<MSqlDatabase*>::iterator it = m_pool.begin();

    uint purgedConnections = 0, totalConnections = 0;
    MSqlDatabase *newDb = NULL;
    while (it != m_pool.end())
    {
        totalConnections++;
        if ((*it)->m_lastDBKick.secsTo(now) <= (int)kPurgeTimeout)
        {
            ++it;
            continue;
        }

        // This connection has not been used in the kPurgeTimeout
        // seconds close it.
        MSqlDatabase *entry = *it;
        it = m_pool.erase(it);
        --m_connCount;
        purgedConnections++;

        // Qt's MySQL driver apparently keeps track of the number of
        // open DB connections, and when it hits 0, calls
        // my_thread_global_end().  The mysql library then assumes the
        // application is ending and that all threads that created DB
        // connections have already exited.  This is rarely true, and
        // may result in the mysql library pausing 5 seconds and
        // printing a message like "Error in my_thread_global_end(): 1
        // threads didn't exit".  This workaround simply creates an
        // extra DB connection before all pooled connections are
        // purged so that my_thread_global_end() won't be called.
        if (it == m_pool.end() &&
            purgedConnections > 0 &&
            totalConnections == purgedConnections)
        {
            newDb = new MSqlDatabase("DBManager" +
                                     QString::number(m_nextConnID++));
            ++m_connCount;
            LOG(VB_GENERAL, LOG_INFO,
                    QString("New DB connection, total: %1").arg(m_connCount));
            newDb->m_lastDBKick = QDateTime::currentDateTime();
        }

        LOG(VB_DATABASE, LOG_INFO, "Deleting idle DB connection...");
        delete entry;
        LOG(VB_DATABASE, LOG_INFO, "Done deleting idle DB connection.");
    }
    if (newDb)
        m_pool.push_front(newDb);

    if (purgedConnections)
    {
        LOG(VB_DATABASE, LOG_INFO,
                QString("Purged %1 idle of %2 total DB connections.")
                .arg(purgedConnections).arg(totalConnections));
    }
}

MSqlDatabase *MDBManager::getStaticCon(MSqlDatabase **dbcon, QString name)
{
    if (!dbcon)
        return NULL;

    if (!*dbcon)
    {
        *dbcon = new MSqlDatabase(name);
        LOG(VB_GENERAL, LOG_INFO, "New static DB connection" + name);
    }

    (*dbcon)->OpenDatabase();

    return *dbcon;
}

MSqlDatabase *MDBManager::getSchedCon()
{
    return getStaticCon(&m_schedCon, "SchedCon");
}

MSqlDatabase *MDBManager::getDDCon()
{
    return getStaticCon(&m_DDCon, "DataDirectCon");
}

MSqlDatabase *MDBManager::getLogCon()
{
    return getStaticCon(&m_LogCon, "LogCon");
}

void MDBManager::closeStaticCon(MSqlDatabase **dbcon)
{
    if (dbcon && *dbcon)
    {
        delete *dbcon;
        *dbcon = NULL;
    }
}

void MDBManager::closeSchedCon()
{
    closeStaticCon(&m_schedCon);
}

void MDBManager::closeDDCon()
{
    closeStaticCon(&m_DDCon);
}

void MDBManager::closeLogCon()
{
    closeStaticCon(&m_LogCon);
}

// Dangerous. Should only be used when the database connection has errored?

void MDBManager::CloseDatabases()
{
    m_lock.lock();

    QList<MSqlDatabase*>::const_iterator it = m_pool.begin();
    MSqlDatabase *db;

    while (it != m_pool.end())
    {
        db = *it;
        LOG(VB_GENERAL, LOG_INFO,
                "Closing DB connection named '" + db->m_name + "'");
        db->m_db.close();
        ++it;
    }

    m_lock.unlock();
}


// -----------------------------------------------------------------------

static void InitMSqlQueryInfo(MSqlQueryInfo &qi)
{
    qi.db = NULL;
    qi.qsqldb = QSqlDatabase();
    qi.returnConnection = true;
}


MSqlQuery::MSqlQuery(const MSqlQueryInfo &qi)
         : QSqlQuery(QString::null, qi.qsqldb)
{
    m_isConnected = false;
    m_db = qi.db;
    m_returnConnection = qi.returnConnection;

    m_isConnected = m_db && m_db->isOpen();

#ifdef DEBUG_QT4_PORT
    m_testbindings = QRegExp("(:\\w+)\\W.*\\1\\b");
#endif
}

MSqlQuery::~MSqlQuery()
{
    if (m_returnConnection)
    {
        MDBManager *dbmanager = GetMythDB()->GetDBManager();

        if (dbmanager && m_db)
        {
            dbmanager->pushConnection(m_db);
        }
    }
}

MSqlQueryInfo MSqlQuery::InitCon()
{
    MSqlDatabase *db = GetMythDB()->GetDBManager()->popConnection();
    MSqlQueryInfo qi;

    InitMSqlQueryInfo(qi);


    // Bootstrapping without a database?
    //if (db->pretendHaveDB)
    if (db->m_db.hostName().isEmpty())
    {
        // Return an invalid database so that QSqlQuery does nothing.
        // Also works around a Qt4 bug where QSqlQuery::~QSqlQuery
        // calls QMYSQLResult::cleanup() which uses mysql_next_result()

        GetMythDB()->GetDBManager()->pushConnection(db);
        qi.returnConnection = false;
        return qi;
    }

    if (db)
    {
        qi.db = db;
        qi.qsqldb = db->db();

        db->KickDatabase();
    }

    return qi;
}

MSqlQueryInfo MSqlQuery::SchedCon()
{
    MSqlDatabase *db = GetMythDB()->GetDBManager()->getSchedCon();
    MSqlQueryInfo qi;

    InitMSqlQueryInfo(qi);
    qi.returnConnection = false;

    if (db)
    {
        qi.db = db;
        qi.qsqldb = db->db();

        db->KickDatabase();
    }

    return qi;
}

MSqlQueryInfo MSqlQuery::DDCon()
{
    MSqlDatabase *db = GetMythDB()->GetDBManager()->getDDCon();
    MSqlQueryInfo qi;

    InitMSqlQueryInfo(qi);
    qi.returnConnection = false;

    if (db)
    {
        qi.db = db;
        qi.qsqldb = db->db();

        db->KickDatabase();
    }

    return qi;
}

MSqlQueryInfo MSqlQuery::LogCon()
{
    MSqlDatabase *db = GetMythDB()->GetDBManager()->getLogCon();
    MSqlQueryInfo qi;

    InitMSqlQueryInfo(qi);
    qi.returnConnection = false;

    if (db)
    {
        qi.db = db;
        qi.qsqldb = db->db();

        db->KickDatabase();
    }

    return qi;
}

void MSqlQuery::CloseSchedCon()
{
    GetMythDB()->GetDBManager()->closeSchedCon();
}

void MSqlQuery::CloseDDCon()
{
    GetMythDB()->GetDBManager()->closeDDCon();
}

void MSqlQuery::CloseLogCon()
{
    GetMythDB()->GetDBManager()->closeLogCon();
}


bool MSqlQuery::exec()
{
    // Database connection down.  Try to restart it, give up if it's still
    // down
    if (!m_db)
    {
        // Database structure's been deleted
        return false;
    }

    if (!m_db->isOpen() && !m_db->Reconnect())
    {
        LOG(VB_GENERAL, LOG_INFO, "MySQL server disconnected");
        return false;
    }

    bool result = QSqlQuery::exec();

    // if the query failed with "MySQL server has gone away"
    // Close and reopen the database connection and retry the query if it
    // connects again
    if (!result && QSqlQuery::lastError().number() == 2006 && m_db->Reconnect())
        result = QSqlQuery::exec();

    if (VERBOSE_LEVEL_CHECK(VB_DATABASE, LOG_DEBUG))
    {
        QString str = lastQuery();

        // Database logging will cause an infinite loop here if not filtered
        // out
        if (!str.startsWith("INSERT INTO logging "))
        {
       	    // Sadly, neither executedQuery() nor lastQuery() display
            // the values in bound queries against a MySQL5 database.
            // So, replace the named placeholders with their values.

            QMapIterator<QString, QVariant> b = boundValues();
            while (b.hasNext())
            {
                b.next();
                str.replace(b.key(), '\'' + b.value().toString() + '\'');
            }

            LOG(VB_DATABASE, LOG_DEBUG,
                QString("MSqlQuery::exec(%1) %2%3")
                        .arg(m_db->MSqlDatabase::GetConnectionName()).arg(str)
                        .arg(isSelect() ? QString(" <<<< Returns %1 row(s)")
                                              .arg(size()) : QString()));
        }
    }

    return result;
}

bool MSqlQuery::exec(const QString &query)
{
    // Database connection down.  Try to restart it, give up if it's still
    // down
    if (!m_db->isOpen() && !m_db->Reconnect())
    {
        LOG(VB_GENERAL, LOG_INFO, "MySQL server disconnected");
        return false;
    }

    bool result = QSqlQuery::exec(query);

    // if the query failed with "MySQL server has gone away"
    // Close and reopen the database connection and retry the query if it
    // connects again
    if (!result && QSqlQuery::lastError().number() == 2006 && m_db->Reconnect())
        result = QSqlQuery::exec(query);

    LOG(VB_DATABASE, LOG_DEBUG,
            QString("MSqlQuery::exec(%1) %2%3")
                    .arg(m_db->MSqlDatabase::GetConnectionName()).arg(query)
                    .arg(isSelect() ? QString(" <<<< Returns %1 row(s)")
                                          .arg(size()) : QString()));

    return result;
}

bool MSqlQuery::next()
{
    bool result = QSqlQuery::next();

    if (result && VERBOSE_LEVEL_CHECK(VB_DATABASE, LOG_DEBUG))
    {
        QString str;
        QSqlRecord record=QSqlQuery::record();

        for ( long int i = 0; i<record.count(); i++ )
        {
            if (!str.isEmpty())
                str.append(", ");

            str.append(record.fieldName(i) + " = " + value(i).toString());
        }

        LOG(VB_DATABASE, LOG_DEBUG,
                QString("MSqlQuery::next(%1) Result: \"%2\"")
                        .arg(m_db->MSqlDatabase::GetConnectionName())
                        .arg(str));
    }

    return result;
}

bool MSqlQuery::prepare(const QString& query)
{
    m_last_prepared_query = query;
#ifdef DEBUG_QT4_PORT
    if (query.contains(m_testbindings))
    {
        LOG(VB_GENERAL, LOG_DEBUG,
                QString("\n\nQuery contains bind value \"%1\" twice:\n\n\n")
                .arg(m_testbindings.cap(1)) + query);
#if 0
        exit(1);
#endif
    }
#endif

    // Database connection down.  Try to restart it, give up if it's still
    // down
    if (!m_db)
    {
        // Database structure has been deleted...
        return false;
    }

    if (!m_db->isOpen() && !m_db->Reconnect())
    {
        LOG(VB_GENERAL, LOG_INFO, "MySQL server disconnected");
        return false;
    }

    bool ok = QSqlQuery::prepare(query);

    // if the prepare failed with "MySQL server has gone away"
    // Close and reopen the database connection and retry the query if it
    // connects again
    if (!ok && QSqlQuery::lastError().number() == 2006 && m_db->Reconnect())
        ok = QSqlQuery::prepare(query);

    if (!ok && !(GetMythDB()->SuppressDBMessages()))
    {
        LOG(VB_GENERAL, LOG_ERR,
            QString("Error preparing query: %1").arg(query));
        LOG(VB_GENERAL, LOG_ERR,
            MythDB::DBErrorMessage(QSqlQuery::lastError()));
    }

    return ok;
}

bool MSqlQuery::testDBConnection()
{
    MSqlDatabase *db = GetMythDB()->GetDBManager()->popConnection();

    // popConnection() has already called OpenDatabase(),
    // so we only have to check if it was successful:
    bool isOpen = db->isOpen();

    GetMythDB()->GetDBManager()->pushConnection(db);
    return isOpen;
}

void MSqlQuery::bindValue (const QString  & placeholder,
                           const QVariant & val, QSql::ParamType paramType)
{
#ifdef DEBUG_QT4_PORT
    // XXX - HACK BEGIN
    // qt4 doesn't like to bind values without occurrence in the prepared query
    if (!m_last_prepared_query.contains(placeholder))
    {
        LOG(VB_GENERAL, LOG_ERR, "Trying to bind a value to placeholder " +
                placeholder + " without occurrence in the prepared query."
                " Ignoring it.\nQuery was: \"" + m_last_prepared_query + "\"");
        return;
    }
    // XXX - HACK END
#endif

    if (val.type() == QVariant::String && val.isNull())
    {
        QSqlQuery::bindValue(placeholder, QString(""), paramType);
        return;
    }
    QSqlQuery::bindValue(placeholder, val, paramType);
}

void MSqlQuery::bindValue(int pos, const QVariant & val,
                                   QSql::ParamType  paramType)
{
    if (val.type() == QVariant::String && val.isNull())
    {
        QSqlQuery::bindValue(pos, QString(""), paramType);
        return;
    }
    QSqlQuery::bindValue(pos, val, paramType);
}

void MSqlQuery::bindValues(MSqlBindings &bindings)
{
    MSqlBindings::Iterator it;
    for (it = bindings.begin(); it != bindings.end(); ++it)
    {
        bindValue(it.key(), it.value());
    }
}

QVariant MSqlQuery::lastInsertId()
{
    return QSqlQuery::lastInsertId();
}

void MSqlAddMoreBindings(MSqlBindings &output, MSqlBindings &addfrom)
{
    MSqlBindings::Iterator it;
    for (it = addfrom.begin(); it != addfrom.end(); ++it)
    {
        output.insert(it.key(), it.value());
    }
}

struct Holder {
    Holder( const QString& hldr = QString::null, int pos = -1 )
        : holderName( hldr ), holderPos( pos ) {}

    bool operator==( const Holder& h ) const
        { return h.holderPos == holderPos && h.holderName == holderName; }
    bool operator!=( const Holder& h ) const
        { return h.holderPos != holderPos || h.holderName != holderName; }
    QString holderName;
    int     holderPos;
};

void MSqlEscapeAsAQuery(QString &query, MSqlBindings &bindings)
{
    MSqlQuery result(MSqlQuery::InitCon());

    QString q = query;
    QRegExp rx(QString::fromLatin1("'[^']*'|:([a-zA-Z0-9_]+)"));

    QVector<Holder> holders;

    int i = 0;
    while ((i = rx.indexIn(q, i)) != -1)
    {
        if (!rx.cap(1).isEmpty())
            holders.append(Holder(rx.cap(0), i));
        i += rx.matchedLength();
    }

    q = query;
    QVariant val;
    QString holder;

    for (i = (int)holders.count() - 1; i >= 0; --i)
    {
        holder = holders[(uint)i].holderName;
        val = bindings[holder];
        QSqlField f("", val.type());
        if (val.isNull())
            f.clear();
        else
            f.setValue(val);

        query = query.replace((uint)holders[(uint)i].holderPos, holder.length(),
                              result.driver()->formatValue(f));
    }
}

