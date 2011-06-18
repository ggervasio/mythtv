#include <iostream>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>

using namespace std;

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSize>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <QString>
#include <QCoreApplication>

#include "mythcommandlineparser.h"
#include "exitcodes.h"
#include "mythconfig.h"
#include "mythcontext.h"
#include "mythverbose.h"
#include "mythversion.h"
#include "util.h"

const int kEnd          = 0,
          kEmpty        = 1,
          kOptOnly      = 2,
          kOptVal       = 3,
          kArg          = 4,
          kPassthrough  = 5,
          kInvalid      = 6;

const char* NamedOptType(int type);

const char* NamedOptType(int type)
{
    if (type == kEnd)
        return "kEnd";
    else if (type == kEmpty)
        return "kEmpty";
    else if (type == kOptOnly)
        return "kOptOnly";
    else if (type == kOptVal)
        return "kOptVal";
    else if (type == kArg)
        return "kArg";
    else if (type == kPassthrough)
        return "kPassthrough";
    else if (type == kInvalid)
        return "kInvalid";

    return "kUnknown";
}

typedef struct helptmp {
    QString left;
    QString right;
    QStringList arglist;
    CommandLineArg arg;
} HelpTmp;

MythCommandLineParser::MythCommandLineParser(QString appname) :
    m_appname(appname), m_allowExtras(false), m_allowPassthrough(false), 
    m_passthroughActive(false), m_overridesImported(false), m_verbose(false)
{
    char *verbose = getenv("VERBOSE_PARSER");
    if (verbose != NULL)
    {
        cerr << "MythCommandLineParser is now operating verbosely." << endl;
        m_verbose = true;
    }

    LoadArguments();
}

void MythCommandLineParser::add(QStringList arglist, QString name,
                                QVariant::Type type, QVariant def,
                                QString help, QString longhelp)
{
    CommandLineArg arg;
    arg.name     = name;
    arg.type     = type;
    arg.def      = def;
    arg.help     = help;
    arg.longhelp = longhelp;

    QStringList::const_iterator i;
    for (i = arglist.begin(); i != arglist.end(); ++i)
    {
        if (!m_registeredArgs.contains(*i))
        {
            if (m_verbose)
                cerr << "Adding " << (*i).toLocal8Bit().constData()
                     << " as taking type '" << QVariant::typeToName(type)
                     << "'" << endl;
            m_registeredArgs.insert(*i, arg);
        }
    }

    if (!m_defaults.contains(arg.name))
        m_defaults[arg.name] = arg.def;
}

void MythCommandLineParser::PrintVersion(void)
{
    cout << "Please attach all output as a file in bug reports." << endl;
    cout << "MythTV Version : " << MYTH_SOURCE_VERSION << endl;
    cout << "MythTV Branch : " << MYTH_SOURCE_PATH << endl;
    cout << "Network Protocol : " << MYTH_PROTO_VERSION << endl;
    cout << "Library API : " << MYTH_BINARY_VERSION << endl;
    cout << "QT Version : " << QT_VERSION_STR << endl;
#ifdef MYTH_BUILD_CONFIG
    cout << "Options compiled in:" <<endl;
    cout << MYTH_BUILD_CONFIG << endl;
#endif
}

void MythCommandLineParser::PrintHelp(void)
{
    QString help = GetHelpString(true);
    cerr << help.toLocal8Bit().constData();
}

QString MythCommandLineParser::GetHelpString(bool with_header) const
{
    QString helpstr;
    QTextStream msg(&helpstr, QIODevice::WriteOnly);

    if (with_header)
    {
        QString versionStr = QString("%1 version: %2 [%3] www.mythtv.org")
            .arg(m_appname).arg(MYTH_SOURCE_PATH).arg(MYTH_SOURCE_VERSION);
        msg << versionStr << endl;

        QString descr = GetHelpHeader();
        if (descr.size() > 0)
            msg << endl << descr << endl << endl;
    }

    if (toString("showhelp").isEmpty())
    {
        // build generic help text
        if (with_header)
            msg << "Valid options are: " << endl;

        QMap<QString, HelpTmp> argmap;
        QString argname;
        HelpTmp help;

        QMap<QString, CommandLineArg>::const_iterator i;
        for (i = m_registeredArgs.begin(); i != m_registeredArgs.end(); ++i)
        {
            if (i.value().help.isEmpty())
                // ignore any arguments with no help text
                continue;

            argname = i.value().name;
            if (argmap.contains(argname))
                argmap[argname].arglist << i.key();
            else
            {
                help.arglist = QStringList(i.key());
                help.arg = i.value();
                argmap[argname] = help;
            }
        }

        int len, maxlen = 0;
        QMap<QString, HelpTmp>::iterator i2;
        for (i2 = argmap.begin(); i2 != argmap.end(); ++i2)
        {
            (*i2).left = (*i2).arglist.join(" OR ");
            (*i2).right = (*i2).arg.help;
            len = (*i2).left.length();
            maxlen = max(len, maxlen);
        }

        maxlen += 4;
        QString pad;
        pad.fill(' ', maxlen);
        QStringList rlist;
        QStringList::const_iterator i3;

        for (i2 = argmap.begin(); i2 != argmap.end(); ++i2)
        {
            msg << (*i2).left.leftJustified(maxlen, ' ');

            rlist = (*i2).right.split('\n');
            wrapList(rlist, 79-maxlen);
            msg << rlist[0] << endl;

            for (i3 = rlist.begin() + 1; i3 != rlist.end(); ++i3)
                msg << pad << *i3 << endl;
        }
    }
    else
    {
        // build help for a specific argument
        QString optstr = "-" + toString("showhelp");
        if (!m_registeredArgs.contains(optstr))
        {
            optstr = "-" + optstr;
            if (!m_registeredArgs.contains(optstr))
                return QString("Could not find option matching '%1'\n")
                            .arg(toString("showhelp"));
        }

        if (with_header)
            msg << "Option:      " << optstr << endl << endl;

        // pull option information, and find aliased options
        CommandLineArg option = m_registeredArgs[optstr];
        QMap<QString, CommandLineArg>::const_iterator cmi;
        QStringList aliases;
        for (cmi = m_registeredArgs.begin(); cmi != m_registeredArgs.end(); ++cmi)
        {
            if (cmi.key() == optstr)
                continue;

            if (cmi.value().name == option.name)
                aliases << cmi.key();
        }

        QStringList::const_iterator sli;
        if (!aliases.isEmpty())
        {
            sli = aliases.begin();
            msg <<     "Aliases:     " << *sli << endl;
            while (++sli != aliases.end())
                msg << "             " << *sli << endl;
        }

        msg << "Type:        " << QVariant::typeToName(option.type) << endl;
        if (option.def.canConvert(QVariant::String))
            msg << "Default:     " << option.def.toString() << endl;

        QStringList help;
        if (!option.longhelp.isEmpty())
            help = option.longhelp.split("\n");
        else
            help = option.help.split("\n");

        sli = help.begin();
        msg << "Description: " << *sli << endl;
        while (++sli != help.end())
            msg << "             " << *sli << endl;
    }

    msg.flush();
    return helpstr;
}

int MythCommandLineParser::getOpt(int argc, const char * const * argv,
                                    int &argpos, QString &opt, QString &val)
{
    opt.clear();
    val.clear();

    if (argpos >= argc)
        // this shouldnt happen, return and exit
        return kEnd;

    QString tmp = QString::fromLocal8Bit(argv[argpos]);
    if (tmp.isEmpty())
        // string is empty, return and loop
        return kEmpty;

    if (m_passthroughActive)
    {
        // pass through has been activated
        val = tmp;
        return kArg;
    }

    if (tmp.startsWith("-") && tmp.size() > 1)
    {
        if (tmp == "--")
        {
            // all options beyond this will be passed as a single string
            m_passthroughActive = true;
            return kPassthrough;
        }

        if (tmp.contains("="))
        {
            // option contains '=', split
            QStringList slist = tmp.split("=");

            if (slist.size() != 2)
            {
                // more than one '=' in option, this is not handled
                opt = tmp;
                return kInvalid;
            }

            opt = slist[0];
            val = slist[1];
            return kOptVal;
        }

        opt = tmp;

        if (argpos+1 >= argc)
            // end of input, option only
            return kOptOnly;

        tmp = QString::fromLocal8Bit(argv[++argpos]);
        if (tmp.isEmpty())
            // empty string, option only
            return kOptOnly;

        if (tmp.startsWith("-") && tmp.size() > 1)
        {
            // no value found for option, backtrack
            argpos--;
            return kOptOnly;
        }

        val = tmp;
        return kOptVal;
    }
    else
    {
        // input is not an option string, return as arg
        val = tmp;
        return kArg;
    }

}

bool MythCommandLineParser::Parse(int argc, const char * const * argv)
{
    bool processed;
    int res;
    QString opt, val;
    CommandLineArg argdef;

    if (m_allowExtras)
        m_parsed["extra"] = QVariant(QVariantMap());

    QMap<QString, CommandLineArg>::const_iterator i;
    for (int argpos = 1; argpos < argc; ++argpos)
    {

        res = getOpt(argc, argv, argpos, opt, val);

        if (m_verbose)
            cerr << "res: " << NamedOptType(res) << endl
                 << "opt: " << opt.toLocal8Bit().constData() << endl
                 << "val: " << val.toLocal8Bit().constData() << endl << endl;

        if (res == kPassthrough && !m_allowPassthrough)
        {
            cerr << "Received '--' but passthrough has not been enabled" << endl;
            return false;
        }

        if (res == kEnd)
            break;
        else if (res == kEmpty)
            continue;
        else if (res == kInvalid)
        {
            cerr << "Invalid option received:" << endl << "    "
                 << opt.toLocal8Bit().constData();
            return false;
        }
        else if (m_passthroughActive)
        {
            m_passthrough << val;
            continue;
        }
        else if (res == kArg)
        {
            m_remainingArgs << val;
            continue;
        }

        // this line should not be passed once arguments have started collecting
        if (!m_remainingArgs.empty())
        {
            cerr << "Command line arguments received out of sequence"
                 << endl;
            return false;
        }

#ifdef Q_WS_MACX
        if (opt.startsWith("-psn_"))
        {
            cerr << "Ignoring Process Serial Number from command line"
                 << endl;
            continue;
        }
#endif

        // scan for matching option handler
        processed = false;
        for (i = m_registeredArgs.begin(); i != m_registeredArgs.end(); ++i)
        {
            if (opt == i.key())
            {
                argdef = i.value();
                processed = true;
                break;
            }
        }

        // if unhandled, and extras are allowed, specify general collection pool
        if (!processed)
        {
            if (m_allowExtras)
            {
                argdef.name = "extra";
                argdef.type = QVariant::Map;
                QString tmp = QString("%1=%2").arg(opt).arg(val);
                val = tmp;
                res = kOptVal;
            }
            else
            {
                // else fault
                cerr << "Unhandled option given on command line:" << endl 
                     << "    " << opt.toLocal8Bit().constData() << endl;
                return false;
            }
        }

        if (res == kOptOnly)
        {
            if (argdef.type == QVariant::Bool)
                m_parsed[argdef.name] = QVariant(!(i.value().def.toBool()));
            else if (argdef.type == QVariant::Int)
            {
                if (m_parsed.contains(argdef.name))
                    m_parsed[argdef.name] = m_parsed[argdef.name].toInt() + 1;
                else
                    m_parsed[argdef.name] = QVariant((i.value().def.toInt())+1);
            }
            else if (argdef.type == QVariant::String)
                m_parsed[argdef.name] = i.value().def;
            else
            {
                cerr << "Command line option did not receive value:" << endl
                     << "    " << opt.toLocal8Bit().constData() << endl;
                return false;
            }
        }
        else if (res == kOptVal)
        {
            if (argdef.type == QVariant::Bool)
            {
                cerr << "Boolean type options do not accept values:" << endl
                     << "    " << opt.toLocal8Bit().constData() << endl;
                return false;
            }
            else if (argdef.type == QVariant::String)
                m_parsed[argdef.name] = QVariant(val);
            else if (argdef.type == QVariant::Int)
                m_parsed[argdef.name] = QVariant(val.toInt());
            else if (argdef.type == QVariant::UInt)
                m_parsed[argdef.name] = QVariant(val.toUInt());
            else if (argdef.type == QVariant::LongLong)
                m_parsed[argdef.name] = QVariant(val.toLongLong());
            else if (argdef.type == QVariant::Double)
                m_parsed[argdef.name] = QVariant(val.toDouble());
            else if (argdef.type == QVariant::StringList)
            {
                QStringList slist;
                if (m_parsed.contains(argdef.name))
                    slist = m_parsed[argdef.name].toStringList();
                slist << val;
                m_parsed[argdef.name] = QVariant(slist);
            }
            else if (argdef.type == QVariant::Map)
            {
                // check for missing key/val pair
                if (!val.contains("="))
                {
                    cerr << "Command line option did not get expected "
                            "key/value pair" << endl;
                    return false;
                }

                QStringList slist = val.split("=");
                QVariantMap vmap;
                if (m_parsed.contains(argdef.name))
                    vmap = m_parsed[argdef.name].toMap();
                vmap[slist[0]] = QVariant(slist[1]);
                m_parsed[argdef.name] = QVariant(vmap);
            }
            else if (argdef.type == QVariant::Size)
            {
                if (!val.contains("x"))
                {
                    cerr << "Command line option did not get expected "
                            "XxY pair" << endl;
                    return false;
                }
                QStringList slist = val.split("x");
                m_parsed[argdef.name] = QSize(slist[0].toInt(), slist[1].toInt());
            }
            else
                m_parsed[argdef.name] = QVariant(val);
        }
    }

    if (m_verbose)
    {
        cerr << "Processed option list:" << endl;
        QMap<QString, QVariant>::const_iterator it = m_parsed.begin();
        for (; it != m_parsed.end(); it++)
        {
            cerr << "  " << it.key().leftJustified(30)
                              .toLocal8Bit().constData();
            if ((*it).type() == QVariant::Bool)
                cerr << ((*it).toBool() ? "True" : "False");
            else if ((*it).type() == QVariant::Int)
                cerr << (*it).toInt();
            else if ((*it).type() == QVariant::UInt)
                cerr << (*it).toUInt();
            else if ((*it).type() == QVariant::LongLong)
                cerr << (*it).toLongLong();
            else if ((*it).type() == QVariant::Double)
                cerr << (*it).toDouble();
            else if ((*it).type() == QVariant::Size)
            {
                QSize tmpsize = (*it).toSize();
                cerr <<  "x=" << tmpsize.width()
                     << " y=" << tmpsize.height();
            }
            else if ((*it).type() == QVariant::String)
                cerr << '"' << (*it).toString().toLocal8Bit()
                                    .constData()
                     << '"';
            else if ((*it).type() == QVariant::StringList)
                cerr << '"' << (*it).toStringList().join("\", \"")
                                    .toLocal8Bit().constData()
                     << '"';
            else if ((*it).type() == QVariant::Map)
            {
                QMap<QString, QVariant> tmpmap = (*it).toMap();
                bool first = true;
                QMap<QString, QVariant>::const_iterator it2 = tmpmap.begin();
                for (; it2 != tmpmap.end(); it2++)
                {
                    if (first)
                        first = false;
                    else
                        cerr << QString("").leftJustified(32)
                                           .toLocal8Bit().constData();
                    cerr << it2.key().toLocal8Bit().constData()
                         << '='
                         << (*it2).toString().toLocal8Bit().constData()
                         << endl;
                }
                continue;
            }
            else if ((*it).type() == QVariant::DateTime)
                cerr << (*it).toDateTime().toString(Qt::ISODate)
                             .toLocal8Bit().constData();
            cerr << endl;
        }

        cerr << endl << "Extra argument list:" << endl;
        QStringList::const_iterator it3 = m_remainingArgs.begin();
        for (; it3 != m_remainingArgs.end(); it3++)
            cerr << "  " << (*it3).toLocal8Bit().constData() << endl;

        if (m_allowPassthrough)
        {
            cerr << endl << "Passthrough string:" << endl;
            cerr << "  " << GetPassthrough().toLocal8Bit().constData() << endl;
        }

        cerr << endl;
    }

    return true;
}

QVariant MythCommandLineParser::operator[](const QString &name)
{
    QVariant res("");
    if (m_parsed.contains(name))
        res = m_parsed[name];
    else if (m_defaults.contains(name))
        res = m_defaults[name];
    return res;
}

QMap<QString,QString> MythCommandLineParser::GetSettingsOverride(void)
{
    QMap<QString,QString> smap;
    if (!m_parsed.contains("overridesettings"))
        return smap;

    QVariantMap vmap = m_parsed["overridesettings"].toMap();

    if (!m_overridesImported)
    {
        if (m_parsed.contains("overridesettingsfile"))
        {
            QString filename = m_parsed["overridesettingsfile"].toString();
            if (!filename.isEmpty())
            {
                QFile f(filename);
                if (f.open(QIODevice::ReadOnly))
                {
                    char buf[1024];
                    int64_t len = f.readLine(buf, sizeof(buf) - 1);
                    while (len != -1)
                    {
                        if (len >= 1 && buf[len-1]=='\n')
                            buf[len-1] = 0;
                        QString line(buf);
                        QStringList tokens = line.split("=",
                                QString::SkipEmptyParts);
                        if (tokens.size() == 2)
                        {
                            tokens[0].replace(QRegExp("^[\"']"), "");
                            tokens[0].replace(QRegExp("[\"']$"), "");
                            tokens[1].replace(QRegExp("^[\"']"), "");
                            tokens[1].replace(QRegExp("[\"']$"), "");
                            if (!tokens[0].isEmpty())
                                vmap[tokens[0]] = QVariant(tokens[1]);
                        }
                        len = f.readLine(buf, sizeof(buf) - 1);
                    }
                    m_parsed["overridesettings"] = QVariant(vmap);
                }
                else
                {
                    QByteArray tmp = filename.toAscii();
                    cerr << "Failed to open the override settings file: '"
                         << tmp.constData() << "'" << endl;
                }
            }
        }
        m_overridesImported = true;
    }

    QVariantMap::const_iterator i;
    for (i = vmap.begin(); i != vmap.end(); ++i)
        smap[i.key()] = i.value().toString();

    // add windowed boolean

    return smap;
}

bool MythCommandLineParser::toBool(QString key) const
{
    // If value is of type boolean, return its value
    // If value is of other type, return whether
    //      it was defined, of if it will return
    //      its default value

    bool val = false;
    if (m_parsed.contains(key))
    {
        if (m_parsed[key].type() == QVariant::Bool)
            val = m_parsed[key].toBool();
        else
            val = true;
    }
    else if (m_defaults.contains(key))
        if (m_defaults[key].type() == QVariant::Bool)
            val = m_defaults[key].toBool();
    return val;
}

int MythCommandLineParser::toInt(QString key) const
{
    // Return matching value if defined, else use default
    // If key is not registered, return 0
    int val = 0;
    if (m_parsed.contains(key))
    {
        if (m_parsed[key].canConvert(QVariant::Int))
            val = m_parsed[key].toInt();
    }
    else if (m_defaults.contains(key))
    {
        if (m_defaults[key].canConvert(QVariant::Int))
            val = m_defaults[key].toInt();
    }
    return val;
}

uint MythCommandLineParser::toUInt(QString key) const
{
    // Return matching value if defined, else use default
    // If key is not registered, return 0
    uint val = 0;
    if (m_parsed.contains(key))
    {
        if (m_parsed[key].canConvert(QVariant::UInt))
            val = m_parsed[key].toUInt();
    }
    else if (m_defaults.contains(key))
    {
        if (m_defaults[key].canConvert(QVariant::UInt))
            val = m_defaults[key].toUInt();
    }
    return val;
}


long long MythCommandLineParser::toLongLong(QString key) const
{
    // Return matching value if defined, else use default
    // If key is not registered, return 0
    long long val = 0;
    if (m_parsed.contains(key))
    {
        if (m_parsed[key].canConvert(QVariant::LongLong))
            val = m_parsed[key].toLongLong();
    }
    else if (m_defaults.contains(key))
    {
        if (m_defaults[key].canConvert(QVariant::LongLong))
            val = m_defaults[key].toLongLong();
    }
    return val;
}

double MythCommandLineParser::toDouble(QString key) const
{
    // Return matching value if defined, else use default
    // If key is not registered, return 0.0
    double val = 0.0;
    if (m_parsed.contains(key))
    {
        if (m_parsed[key].canConvert(QVariant::Double))
            val = m_parsed[key].toDouble();
    }
    else if (m_defaults.contains(key))
    {
        if (m_defaults[key].canConvert(QVariant::Double))
            val = m_defaults[key].toDouble();
    }
    return val;
}

QSize MythCommandLineParser::toSize(QString key) const
{
    // Return matching value if defined, else use default
    // If key is not registered, return (0,0)
    QSize val(0,0);
    if (m_parsed.contains(key))
    {
        if (m_parsed[key].canConvert(QVariant::Size))
            val = m_parsed[key].toSize();
    }
    else if (m_defaults.contains(key))
    {
        if (m_defaults[key].canConvert(QVariant::Size))
            val = m_defaults[key].toSize();
    }
    return val;
}

QString MythCommandLineParser::toString(QString key) const
{
    // Return matching value if defined, else use default
    // If key is not registered, return empty string
    QString val("");
    if (m_parsed.contains(key))
    {
        if (m_parsed[key].canConvert(QVariant::String))
            val = m_parsed[key].toString();
    }
    else if (m_defaults.contains(key))
    {
        if (m_defaults[key].canConvert(QVariant::String))
            val = m_defaults[key].toString();
    }
    return val;
}

QStringList MythCommandLineParser::toStringList(QString key, QString sep) const
{
    // Return matching value if defined, else use default
    // If key is not registered, return empty stringlist
    QStringList val;
    if (m_parsed.contains(key))
    {
        if (m_parsed[key].type() == QVariant::String && !sep.isEmpty())
            val << m_parsed[key].toString().split(sep);
        else if (m_parsed[key].canConvert(QVariant::StringList))
            val << m_parsed[key].toStringList();
    }
    else if (m_defaults.contains(key))
    {
        if (m_defaults[key].type() == QVariant::String && !sep.isEmpty())
            val << m_defaults[key].toString().split(sep);
        else if (m_defaults[key].canConvert(QVariant::StringList))
            val = m_defaults[key].toStringList();
    }
    return val;
}

QMap<QString,QString> MythCommandLineParser::toMap(QString key) const
{
    // Return matching value if defined, else use default
    // If key is not registered, return empty stringmap
    QMap<QString,QString> val;
    if (m_parsed.contains(key))
    {
        if (m_parsed[key].canConvert(QVariant::Map))
        {
            QMap<QString,QVariant> tmp = m_parsed[key].toMap();
            QMap<QString,QVariant>::const_iterator i;
            for (i = tmp.begin(); i != tmp.end(); ++i)
                val[i.key()] = i.value().toString();
        }
    }
    else if (m_defaults.contains(key))
    {
        if (m_defaults[key].canConvert(QVariant::Map))
        {
            QMap<QString,QVariant> tmp = m_defaults[key].toMap();
            QMap<QString,QVariant>::const_iterator i;
            for (i = tmp.begin(); i != tmp.end(); ++i)
                val[i.key()] = i.value().toString();
        }
    }
    return val;
}

QDateTime MythCommandLineParser::toDateTime(QString key) const
{
    // Return matching value if defined, else use default
    // If key is not registered, return empty datetime
    QDateTime val;
    if (m_parsed.contains(key))
    {
        if (m_parsed[key].canConvert(QVariant::DateTime))
            val = m_parsed[key].toDateTime();
    }
    else if (m_defaults.contains(key))
    {
        if (m_defaults[key].canConvert(QVariant::Int))
            val = m_defaults[key].toDateTime();
    }
    return val;
}

void MythCommandLineParser::addHelp(void)
{
    add(QStringList( QStringList() << "-h" << "--help" << "--usage" ),
            "showhelp", "", "Display this help printout.",
            "Displays a list of all commands available for use with "
            "this application. If another option is provided as an "
            "argument, it will provide detailed information on that "
            "option.");
}

void MythCommandLineParser::addVersion(void)
{
    add("--version", "showversion", "Display version information.",
            "Display informtion about build, including:\n"
            " version, branch, protocol, library API, Qt "
            "and compiled options.");
}

void MythCommandLineParser::addWindowed(bool def)
{
    if (def)
        add(QStringList( QStringList() << "-nw" << "--no-windowed" ),
            "notwindowed", "Prevent application from running in window.", "");
    else
        add(QStringList( QStringList() << "-w" << "--windowed" ), "windowed",
            "Force application to run in a window.", "");
}

void MythCommandLineParser::addDaemon(void)
{
    add(QStringList( QStringList() << "-d" << "--daemon" ), "daemon",
            "Fork application into background after startup.",
            "Fork application into background, detatching from "
            "the local terminal.\nOften used with: "
            " --logpath --pidfile --user");
}

void MythCommandLineParser::addSettingsOverride(void)
{
    add(QStringList( QStringList() << "-O" << "--override-setting" ),
            "overridesettings", QVariant::Map,
            "Override a single setting defined by a key=value pair.",
            "Override a single setting from the database using "
            "options defined as one or more key=value pairs\n"
            "Multiple can be defined by multiple uses of the "
            "-O option.");
    add("--override-settings-file", "overridesettingsfile", "", 
            "Define a file of key=value pairs to be "
            "loaded for setting overrides.", "");
}

void MythCommandLineParser::addVerbose(void)
{
    add(QStringList( QStringList() << "-v" << "--verbose" ), "verbose",
            "important,general",
            "Specify log filtering. Use '-v help' for level info.", "");
    add("-V", "verboseint", 0U, "",
            "This option is intended for internal use only.\n"
            "This option takes an unsigned value corresponding "
            "to the bitwise log verbosity operator.");
}

void MythCommandLineParser::addRecording(void)
{
    add("--chanid", "chanid", 0U,
            "Specify chanid of recording to operate on.", "");
    add("--starttime", "starttime", "",
            "Specify start time of recording to operate on.", "");
}

void MythCommandLineParser::addGeometry(void)
{
    add(QStringList( QStringList() << "-geometry" << "--geometry" ), "geometry",
            "", "Specify window size and position (WxH[+X+Y])", "");
}

void MythCommandLineParser::addDisplay(void)
{
#ifdef USING_X11
    add("-display", "display", "", "Specify X server to use.", "");
#endif
}

void MythCommandLineParser::addUPnP(void)
{
    add("--noupnp", "noupnp", "Disable use of UPnP.", "");
}

void MythCommandLineParser::addLogging(void)
{
    add(QStringList( QStringList() << "-l" << "--logfile" << "--logpath" ), 
            "logpath", "",
            "Writes logging messages to a file at logpath.\n"
            "If a directory is given, a logfile will be created in that "
            "directory with a filename of applicationName.pid.log.\n"
            "If a full filename is given, that file will be used.\n"
            "This is typically used in combination with --daemon, and if used "
            "in combination with --pidfile, this can be used with log "
            "rotaters, using the HUP call to inform MythTV to reload the "
            "file (currently disabled).", "");
    add(QStringList( QStringList() << "-q" << "--quiet"), "quiet", 0,
            "Don't log to the console (-q).  Don't log anywhere (-q -q)", "");
    add("--syslog", "syslog", "none", 
            "Set the syslog logging facility.\nSet to \"none\" to disable, "
            "defaults to none", "");
    add("--nodblog", "nodblog", "Disable database logging.", "");
}

void MythCommandLineParser::addPIDFile(void)
{
    add(QStringList( QStringList() << "-p" << "--pidfile" ), "pidfile", "",
            "Write PID of application to filename.",
            "Write the PID of the currently running process as a single "
            "line to this file. Used for init scripts to know what "
            "process to terminate, and with --logfile and log rotaters "
            "to send a HUP signal to process to have it re-open files.");
}

void MythCommandLineParser::addJob(void)
{
    add(QStringList( QStringList() << "-j" << "--jobid" ), "jobid", 0, "",
            "Intended for internal use only, specify the JobID to match "
            "up with in the database for additional information and the "
            "ability to update runtime status in the database.");
}

MythBackendCommandLineParser::MythBackendCommandLineParser() :
    MythCommandLineParser(MYTH_APPNAME_MYTHBACKEND)
{ LoadArguments(); }

void MythBackendCommandLineParser::LoadArguments(void)
{
    addHelp();
    addVersion();
    addDaemon();
    addSettingsOverride();
    addVerbose();
    addUPnP();
    addLogging();
    addPIDFile();

    add("--printsched", "printsched",
            "Print upcoming list of scheduled recordings.", "");
    add("--testsched", "testsched", "do some scheduler testing.", "");
    add("--resched", "resched",
            "Trigger a run of the recording scheduler on the existing "
            "master backend.",
            "This command will connect to the master backend and trigger "
            "a run of the recording scheduler. The call will return "
            "immediately, however the scheduler run may take several "
            "seconds to a minute or longer to complete.");
    add("--nosched", "nosched", "",
            "Intended for debugging use only, disable the scheduler "
            "on this backend if it is the master backend, preventing "
            "any recordings from occuring until the backend is "
            "restarted without this option.");
    add("--scanvideos", "scanvideos",
            "Trigger a rescan of media content in MythVideo.",
            "This command will connect to the master backend and trigger "
            "a run of the Video scanner. The call will return "
            "immediately, however the scanner may take several seconds "
            "to tens of minutes, depending on how much new or moved "
            "content it has to hash, and how quickly the scanner can "
            "access those files to do so. If enabled, this will also "
            "trigger the bulk metadata scanner upon completion.");
    add("--nojobqueue", "nojobqueue", "",
            "Intended for debugging use only, disable the jobqueue "
            "on this backend. As each jobqueue independently selects "
            "jobs, this will only have any affect on this local "
            "backend.");
    add("--nohousekeeper", "nohousekeeper", "",
            "Intended for debugging use only, disable the housekeeper "
            "on this backend if it is the master backend, preventing "
            "any guide processing, recording cleanup, or any other "
            "task performed by the housekeeper.");
    add("--noautoexpire", "noautoexpire", "",
            "Intended for debugging use only, disable the autoexpirer "
            "on this backend if it is the master backend, preventing "
            "recordings from being expired to clear room for new "
            "recordings.");
    add("--event", "event", "", "Send a backend event test message.", "");
    add("--systemevent", "systemevent", "",
            "Send a backend SYSTEM_EVENT test message.", "");
    add("--clearcache", "clearcache",
            "Trigger a cache clear on all connected MythTV systems.",
            "This command will connect to the master backend and trigger "
            "a cache clear event, which will subsequently be pushed to "
            "all other connected programs. This event will clear the "
            "local database settings cache used by each program, causing "
            "options to be re-read from the database upon next use.");
    add("--printexpire", "printexpire", "ALL",
            "Print upcoming list of recordings to be expired.", "");
    add("--setverbose", "setverbose", "",
            "Change debug level of the existing master backend.", "");
    add("--user", "username", "",
            "Drop permissions to username after starting.", "");
}

QString MythBackendCommandLineParser::GetHelpHeader(void) const
{
    return "MythBackend is the primary server application for MythTV. It is \n"
           "used for recording and remote streaming access of media. Only one \n"
           "instance of this application is allowed to run on one host at a \n"
           "time, and one must be configured to operate as a master, performing \n"
           "additional scheduler and housekeeper tasks.";
}

MythFrontendCommandLineParser::MythFrontendCommandLineParser() :
    MythCommandLineParser(MYTH_APPNAME_MYTHFRONTEND)
{ LoadArguments(); }

void MythFrontendCommandLineParser::LoadArguments(void)
{
    addHelp();
    addVersion();
    addWindowed(false);
    addSettingsOverride();
    addVerbose();
    addGeometry();
    addDisplay();
    addUPnP();
    addLogging();

    add(QStringList( QStringList() << "-r" << "--reset" ), "reset",
        "Resets appearance, settings, and language.", "");
    add(QStringList( QStringList() << "-p" << "--prompt" ), "prompt",
        "Always prompt for backend selection.", "");
    add(QStringList( QStringList() << "-d" << "--disable-autodiscovery" ),
        "noautodiscovery", "Prevent frontend from using UPnP autodiscovery.", "");
}

QString MythFrontendCommandLineParser::GetHelpHeader(void) const
{
    return "MythFrontend is the primary playback application for MythTV. It \n"
           "is used for playback of scheduled and live recordings, and management \n"
           "of recording rules.";
}

MythPreviewGeneratorCommandLineParser::MythPreviewGeneratorCommandLineParser() :
    MythCommandLineParser(MYTH_APPNAME_MYTHPREVIEWGEN)
{ LoadArguments(); }

void MythPreviewGeneratorCommandLineParser::LoadArguments(void)
{
    addHelp();
    addVersion();
    addVerbose();
    addRecording();
    addLogging();

    add("--seconds", "seconds", 0LL, "Number of seconds into video to take preview image.", "");
    add("--frame", "frame", 0LL, "Number of frames into video to take preview image.", "");
    add("--size", "size", QSize(0,0), "Dimensions of preview image.", "");
    add("--infile", "inputfile", "", "Input video for preview generation.", "");
    add("--outfile", "outputfile", "", "Optional output file for preview generation.", "");
}

MythWelcomeCommandLineParser::MythWelcomeCommandLineParser() :
    MythCommandLineParser(MYTH_APPNAME_MYTHWELCOME)
{ LoadArguments(); }

QString MythWelcomeCommandLineParser::GetHelpHeader(void) const
{
    return "MythWelcome is a graphical launcher application to allow MythFrontend \n"
           "to disconnect from the backend, and allow automatic shutdown to occur.";
}

void MythWelcomeCommandLineParser::LoadArguments(void)
{
    addHelp();
    addSettingsOverride();
    addVersion();
    addVerbose();
    addLogging();

    add(QStringList( QStringList() << "-s" << "--setup" ), "setup",
            "Run setup for mythshutdown.", "");
}

MythAVTestCommandLineParser::MythAVTestCommandLineParser() :
    MythCommandLineParser(MYTH_APPNAME_MYTHAVTEST)
{ LoadArguments(); }

QString MythAVTestCommandLineParser::GetHelpHeader(void) const
{
    return "MythAVTest is a testing application that allows direct access \n"
           "to the MythTV internal video player.";
}

void MythAVTestCommandLineParser::LoadArguments(void)
{
    addHelp();
    addSettingsOverride();
    addVersion();
    addWindowed(false);
    addVerbose();
    addGeometry();
    addDisplay();
    addLogging();
}

MythCommFlagCommandLineParser::MythCommFlagCommandLineParser() :
    MythCommandLineParser(MYTH_APPNAME_MYTHCOMMFLAG)
{ LoadArguments(); }

void MythCommFlagCommandLineParser::LoadArguments(void)
{
    addHelp();
    addSettingsOverride();
    addVersion();
    addVerbose();
    addJob();
    addRecording();
    addLogging();

    add(QStringList( QStringList() << "-f" << "--file" ), "file", "",
            "Specify file to operate on.", "");
    add("--video", "video", "", "Rebuild the seek table for a video (non-recording) file.", "");
    add("--method", "commmethod", "", "Commercial flagging method[s] to employ:\n"
                                      "off, blank, scene, blankscene, logo, all, "
                                      "d2, d2_logo, d2_blank, d2_scene, d2_all", "");
    add("--outputmethod", "outputmethod", "",
            "Format of output written to outputfile, essentials, full.", "");
    add("--gencutlist", "gencutlist", "Copy the commercial skip list to the cutlist.", "");
    add("--clearcutlist", "clearcutlist", "Clear the cutlist.", "");
    add("--clearskiplist", "clearskiplist", "Clear the commercial skip list.", "");
    add("--getcutlist", "getcutlist", "Display the current cutlist.", "");
    add("--getskiplist", "getskiplist", "Display the current commercial skip list.", "");
    add("--setcutlist", "setcutlist", "", "Set a new cutlist in the form:\n"
                                          "#-#[,#-#]... (ie, 1-100,1520-3012,4091-5094)", "");
    add("--skipdb", "skipdb", "", "Intended for external 3rd party use.");
    add("--queue", "queue", "Insert flagging job into the JobQueue, rather than "
                            "running flagging in the foreground.", "");
    add("--noprogress", "noprogress", "Don't print progress on stdout.", "");
    add("--rebuild", "rebuild", "Do not flag commercials, just rebuild the seektable.", "");
    add("--force", "force", "Force operation, even if program appears to be in use.", "");
    add("--dontwritetodb", "dontwritedb", "", "Intended for external 3rd party use.");
    add("--onlydumpdb", "dumpdb", "", "?");
    add("--outputfile", "outputfile", "", "File to write commercial flagging output [debug].", "");
}

MythJobQueueCommandLineParser::MythJobQueueCommandLineParser() :
    MythCommandLineParser(MYTH_APPNAME_MYTHJOBQUEUE)
{ LoadArguments(); }

QString MythJobQueueCommandLineParser::GetHelpHeader(void) const
{
    return "MythJobqueue is daemon implementing the job queue. It is intended \n"
           "for use as additional processing power without requiring a full backend.";
}

void MythJobQueueCommandLineParser::LoadArguments(void)
{
    addHelp();
    addSettingsOverride();
    addVersion();
    addVerbose();
    addLogging();
    addPIDFile();
    addDaemon();
}

MythFillDatabaseCommandLineParser::MythFillDatabaseCommandLineParser() :
    MythCommandLineParser(MYTH_APPNAME_MYTHFILLDATABASE)
{ LoadArguments(); }

void MythFillDatabaseCommandLineParser::LoadArguments(void)
{
    addHelp();
    addVersion();
    addVerbose();
    addLogging();

    add("--manual", "manual", "Run interactive configuration",
            "Manual mode will interactively ask you questions about "
            "each channel as it is processed, to configure for "
            "future use."
            "mutually exclusive with --update");
    add("--update", "update", "Run non-destructive updates",
            "Run non-destructive updates on the database for "
            "users in xmltv zones that do not provide channel "
            "data. Stops the addition of new channels and the "
            "changing of channel icons."
            "mutually exclusive with --manual");
    add("--preset", "preset", "Use channel preset values instead of numbers",
            "For use with assigning preset numbers for each "
            "channel. Useful for non-US countries where people "
            "are used to assigning a sequenced number for each "
            "channel:\n1->TVE1(S41), 2->La 2(SE18), 3->TV(21)...");
    add("--file", "file", "Bypass grabbers and define sourceid and file",
            "Directly define the sourceid and XMLTV file to "
            "import. Must be used in combination with:"
            " --sourceid  --xmlfile");
    add("--dd-file", "file", "Bypass grabber, and read SD data from file",
            "Directly define the data needed to import a local "
            "DataDirect download. Must be used in combination "
            "with: \n"
            " --sourceid  --lineupid  --offset  --xmlfile");
    add("--sourceid", "sourceid", 0, "Operate on single source",
            "Limit mythfilldatabase to only operate on the "
            "specified channel source. This option is required "
            "when using --file, --dd-file, or --xawchannels.");
    add("--offset", "offset", 0, "Day offset of input xml file"
            "Specify how many days offset from today is the "
            "information in the given XML file. This option is "
            "required when using --dd-file.");
    add("--lineupid", "lineupid", 0, "DataDirect lineup of input xml file"
            "Specify the DataDirect lineup that corresponds to "
            "the information in the given XML file. This option "
            "is required when using --dd-file.");
    add("--xmlfile", "xmlfile", "", "XML file to import manually",
            "Specify an XML guide data file to import directly "
            "rather than pull data through the specified grabber.\n"
            "This option is required when using --file or --dd-file.");
    add("--xawchannels", "xawchannels", "Read channels from xawtvrc file",
            "Import channels from an xawtvrc file.\nThis option "
            "requires --sourceid and --xawtvrcfile.");
    add("--xawtvrcfile", "xawtvrcfile", "", "xawtvrc file to import channels from",
            "Xawtvrc file containing channels to be imported.\n"
            "This option is required when using --xawchannels.");
    add("--do-channel-updates", "dochannelupdates", "update channels using datadirect",
            "When using DataDirect, ask mythfilldatabase to "
            "overwrite channel names, frequencies, etc. with "
            "values available from the data source. This will "
            "override custom channel names, which is why it "
            "is disabled by default.");
    add("--remove-new-channels", "removechannels",
            "disable new channels on datadirect web interface",
            "When using DataDirect, ask mythfilldatabase to "
            "mark any new channels as disabled on the remote "
            "lineup. Channels can be manually enabled on the "
            "website at a later time, and incorporated into "
            "MythTV by running mythfilldatabase without this "
            "option. New digital channels cannot be directly "
            "imported and thus are disabled automatically.");
    add("--do-not-filter-new-channels", "nofilterchannels",
            "don't filter ATSC channels for addition",
            "Normally, MythTV tries to avoid adding ATSC "
            "channels to NTSC channel lineups. This option "
            "restores the behavior of adding every channel in "
            "the downloaded channel lineup to MythTV's lineup, "
            "in case MythTV's smarts fail you.");
    add("--graboptions", "graboptions", "", "",
            "Manually define options to pass to the data grabber. "
            "Do NOT use this option unless you know what you are "
            "doing. Mythfilldatabase will automatically use the "
            "correct options for xmltv compliant grabbers.");
    add("--cardtype", "cardtype", "", "", "No information.");           // need documentation for this one
    add("--max-days", "maxdays", 0, "force number of days to update",
            "Force the maximum number of days, counting today, "
            "for the guide data grabber to check for future "
            "listings.");
    add("--refresh-today", "refreshtoday", "refresh today's listings",
            "This option is only valid for selected grabbers.\n"
            "Force a refresh for today's guide data.\nThis can be used "
            "in combination with other --refresh-<n> options.\n"
            "If being used with datadirect, this option should not be "
            "used, rather use --dd-grab-all to pull all listings each time.");
    add("--dont-refresh-tomorrow", "dontrefreshtomorrow",
            "don't refresh tomorrow's listings",
            "This option is only valid for selected grabbers.\n"
            "Prevent mythfilldatabase from pulling information for "
            "tomorrow's listings. Data for tomorrow is always pulled "
            "unless specifically specified otherwise.\n"
            "If being used with datadirect, this option should not be "
            "used, rather use --dd-grab-all to pull all listings each time.");
    add("--refresh-second", "refreshsecond", "refresh listings two days from now",
            "This option is only valid for selected grabbers.\n"
            "Force a refresh for guide data two days from now. This can "
            "be used in combination with other --refresh-<n> options.\n"
            "If being used with datadirect, this option should not be "
            "used, rather use --dd-grab-all to pull all listings each time.");
    add("--refresh-all", "refreshall", "refresh listings on all days",
            "This option is only valid for selected grabbers.\n"
            "This option forces a refresh of all guide data, but does so "
            "with fourteen downloads of one day each.\n"
            "If being used with datadirect, this option should not be "
            "used, rather use --dd-grab-all to pull all listings each time.");
// TODO: I should be converted to a qstringlist and used in place of
//       the other refresh options
    add("--refresh-day", "refreshday", 0U, "refresh specific day's listings",
            "This option is only valid for selected grabbers.\n"
            "Force a refresh for guide data on a specific day. This can "
            "be used in combination with other --refresh-<n> options.\n"
            "If being used with datadirect, this option should not be "
            "used, rather use --dd-grab-all to pull all listings each time.");
    add("--dont-refresh-tba", "dontrefreshtba",
            "don't refresh \"To be announced\" programs",
            "This option is only valid for selected grabbers.\n"
            "Prevent mythfilldatabase from automatically refreshing any "
            "programs marked as \"To be announced\".\n"
            "If being used with datadirect, this option should not be "
            "used, rather use --dd-grab-all to pull all listings each time.");
    add("--dd-grab-all", "ddgraball", "refresh full data using DataDirect",
            "This option is only valid for selected grabbers (DataDirect).\n"
            "This option is the preferred way of updating guide data from "
            "DataDirect, and pulls all fourteen days of guide data at once.");
    add("--only-update-channels", "onlychannels", "only update channel lineup",
            "Download as little listings data as possible to update the "
            "channel lineup.");
    add("--no-mark-repeats", "markrepeats", true, "do not mark repeats", "");
    add("--export-icon-map", "exporticonmap", "iconmap.xml",
            "export icon map to file", "");
    add("--import-icon-map", "importiconmap", "iconmap.xml",
            "import icon map to file", "");
    add("--update-icon-map", "updateiconmap", "updates icon map icons", "");
    add("--reset-icon-map", "reseticonmap", "", "resets icon maps",
            "Reset all icon maps. If given 'all' as an optional value, reset "
            "channel icons as well.");
}

MythLCDServerCommandLineParser::MythLCDServerCommandLineParser() :
    MythCommandLineParser(MYTH_APPNAME_MYTHLCDSERVER)
{ LoadArguments(); }

void MythLCDServerCommandLineParser::LoadArguments(void)
{
    addHelp();
    addVersion();
    addVerbose();
    addDaemon();
    addLogging();
    //addPIDFile();

    add(QStringList( QStringList() << "-p" << "--port" ), "port", 6545, "listen port",
            "This is the port MythLCDServer will listen on for events.");
    add(QStringList( QStringList() << "-m" << "--startupmessage" ), "message", "",
            "Message to display on startup.", "");
    add(QStringList( QStringList() << "-t" << "--messagetime"), "messagetime", 30,
            "Message display duration (in seconds)", "");
    add(QStringList( QStringList() << "-x" << "--debuglevel" ), "debug", 0,
            "debug verbosity", "Control debugging verbosity, values from 0-10");
}

MythMessageCommandLineParser::MythMessageCommandLineParser() :
    MythCommandLineParser(MYTH_APPNAME_MYTHMESSAGE)
{ LoadArguments(); }

void MythMessageCommandLineParser::LoadArguments(void)
{
    addHelp();
    addVersion();
    addVerbose();
    allowExtras();

    add("--udpport", "port", 6948, "(optional) UDP Port to send to", "");
    add("--bcastaddr", "addr", "127.0.0.1",
            "(optional) IP address to send to", "");
    add("--print-template", "printtemplate",
            "Print the template to be sent to the frontend", "");
}

MythShutdownCommandLineParser::MythShutdownCommandLineParser() :
    MythCommandLineParser(MYTH_APPNAME_MYTHSHUTDOWN)
{ LoadArguments(); }

void MythShutdownCommandLineParser::LoadArguments(void)
{
    addHelp();
    addVersion();
    addVerbose();
    addLogging();

    add(QStringList( QStringList() << "-w" << "--setwakeup" ), "setwakeup", "",
            "Set the wakeup time (yyyy-MM-ddThh:mm:ss)", "");
    add(QStringList( QStringList() << "-t" << "--setscheduledwakeup" ), "setschedwakeup",
            "Set wakeup time to the next scheduled recording", "");
    add(QStringList( QStringList() << "-q" << "--shutdown" ), "shutdown",
            "Apply wakeup time to nvram and shutdown.", "");
    add(QStringList( QStringList() << "-x" << "--safeshutdown" ), "safeshutdown",
            "Check if shutdown is possible, and shutdown", "");
    add(QStringList( QStringList() << "-p" << "--startup" ), "startup",
            "Check startup status",
            "Check startup status\n"
            "   returns 0 - automatic startup\n"
            "           1 - manual startup");
    add(QStringList( QStringList() << "-c" << "--check" ), "check", 1,
            "Check whether shutdown is possible",
            "Check whether shutdown is possible depending on input\n"
            "   input 0 - dont check recording status\n"
            "         1 - do check recording status\n\n"
            " returns 0 - ok to shut down\n"
            "         1 - not ok, idle check reset");
    add(QStringList( QStringList() << "-l" << "--lock" ), "lock",
            "disable shutdown", "");
    add(QStringList( QStringList() << "-u" << "--unlock" ), "unlock",
            "enable shutdown", "");
    add(QStringList( QStringList() << "-s" << "--status" ), "status", 1,
            "check current status",
            "check current status depending on input\n"
            "   input 0 - dont check recording status\n"
            "         1 - do check recording status\n\n"
            " returns 0 - Idle\n"
            "         1 - Transcoding\n"
            "         2 - Commercial Detection\n"
            "         4 - Grabbing EPG data\n"
            "         8 - Recording (only valid if input=1)\n"
            "        16 - Locked\n"
            "        32 - Jobs running or pending\n"
            "        64 - In daily wakeup/shutdown period\n"
            "       128 - Less than 15 minutes to next wakeup period\n"
            "       255 - Setup is running");
}

MythTVSetupCommandLineParser::MythTVSetupCommandLineParser() :
    MythCommandLineParser(MYTH_APPNAME_MYTHTV_SETUP)
{ LoadArguments(); }

QString MythTVSetupCommandLineParser::GetHelpHeader(void) const
{
    return "Mythtv-setup is the setup application for the backend server. It is \n"
           "used to configure the backend, and manage tuner cards and storage. \n"
           "Most settings will require a restart of the backend before they take \n"
           "effect.";
}

void MythTVSetupCommandLineParser::LoadArguments(void)
{
    addHelp();
    addSettingsOverride();
    addVersion();
    addWindowed(false);
    addVerbose();
    addGeometry();
    addDisplay();
    addLogging();

    add("--expert", "expert", "", "Expert mode.");
    add("--scan-list", "scanlist", "", "no help");
    add("--scan-save-only", "savescan", "", "no help");
    add("--scan-non-interactive", "scannoninteractive", "", "nohelp");

    add("--scan", "scan", 0U, "", 
            "Run the command line channel scanner on a specified card "
            "ID. This can be used with --frequency-table and --input-name.");
    add("--frequency-table", "freqtable", "atsc-vsb8-us", "",
            "Specify frequency table to be used with command "
            "line channel scanner.");
    add("--input-name", "inputname", "", "",
            "Specify which input to scan for, if specified card "
            "supports multiple.");

    add("--scan-import", "importscan", 0U, "",
            "Import an existing scan from the database. Use --scan-list "
            "to enumerate scans available for import.\n"
            "This option is mutually exclusive with --scan, and can "
            "be used with the options --FTAonly and --service-type.");
    add("--FTAonly", "ftaonly", "", "Only import 'Free To Air' channels.");
    add("--service-type", "servicetype", "all", "",
            "To be used with channel scanning or importing, specify "
            "the type of services to import. Select from the following, "
            "multiple can be added with '+':\n"
            "   all, tv, radio");
}

MythTranscodeCommandLineParser::MythTranscodeCommandLineParser() :
    MythCommandLineParser(MYTH_APPNAME_MYTHTRANSCODE)
{ LoadArguments(); }

void MythTranscodeCommandLineParser::LoadArguments(void)
{
    addHelp();
    addVersion();
    addVerbose();
    addJob();
    addRecording();
    addSettingsOverride();
    addLogging();

    add(QStringList( QStringList() << "-i" << "--infile" ), "inputfile", "",
            "Input video for transcoding.", "");
    add(QStringList( QStringList() << "-o" << "--outfile" ), "outputfile", "",
            "Optional output file for transcoding.", "");
    add(QStringList( QStringList() << "-p" << "--profile" ), "profile", "",
            "Transcoding profile.", "");
    add(QStringList( QStringList() << "-l" << "--honorcutlist" ), "usecutlist",
            "", "Specifies whether to use the cutlist.",
            "Specifies whether transcode should honor the cutlist and "
            "remove the marked off commercials. Optionally takes a "
            "a cutlist as an argument when used with --infile.");
    add("--inversecut", "inversecut",
            "Inverses the cutlist, leaving only the marked off sections.", "");
    add(QStringList( QStringList() << "--allkeys" << "-k" ), "allkeys",
            "Specifies the outputfile should be entirely keyframes.", "");
    add(QStringList( QStringList() << "-f" << "--fifodir" ), "fifodir", "",
            "Directory in which to write fifos to.", "");
    add("--fifosync", "fifosync", "Enforce fifo sync.", "");
    add("--passthrough", "passthru", "Pass through raw, unprocessed audio.", "");
    add(QStringList( QStringList() << "-b" << "--buildindex" ), "reindex",
            "Build new keyframe index.", "");
    add("--video", "video",
            "Specifies video is not a recording, must use --infile.", "");
    add("--showprogress", "showprogress", "Display status info in stdout", "");
    add(QStringList( QStringList() << "-ro" << "--recorderOptions" ), "recopt",
            "", "Comma separated list of recordingprofile overrides.", "");
    add("--audiotrack", "audiotrack", 0, "Select specific audio track.", "");
    add(QStringList( QStringList() << "-m" << "--mpeg2" ), "mpeg2",
            "Specifies that a lossless transcode should be used.", "");
    add(QStringList( QStringList() << "-e" << "--ostream" ), "ostream", ""
            "Output stream type: dvd, ps", "");
}

MythMediaServerCommandLineParser::MythMediaServerCommandLineParser() :
    MythCommandLineParser(MYTH_APPNAME_MYTHMEDIASERVER)
{ LoadArguments(); }

QString MythMediaServerCommandLineParser::GetHelpHeader(void) const
{
    return "MythMediaServer is daemon implementing the backend file server. \n"
           "It is intended to allow access to remote file storage on machines \n"
           "that do not have tuners, and as such cannot run a backend.";
}

void MythMediaServerCommandLineParser::LoadArguments(void)
{
    addHelp();
    addVersion();
    addVerbose();
    addSettingsOverride();
    addPIDFile();
    addDaemon();
    addLogging();
}


QString MythCommandLineParser::GetLogFilePath(void)
{
    QString logfile = toString("logpath");
    pid_t   pid = getpid();

    if (logfile.isEmpty())
        return logfile;

    QString logdir;
    QString filepath;

    QFileInfo finfo(logfile);
    if (finfo.isDir())
    {
        logdir  = finfo.filePath();
        logfile = QCoreApplication::applicationName() + 
                  QString(".%1").arg(pid) + ".log";
    }
    else
    {
        logdir  = finfo.path();
        logfile = finfo.fileName();
    }

    m_parsed.insert("logdir", logdir);
    m_parsed.insert("logfile", logfile);
    m_parsed.insert("filepath", QFileInfo(QDir(logdir), logfile).filePath());

    return toString("filepath");
}

int MythCommandLineParser::GetSyslogFacility(void)
{
    QString setting = toString("syslog").toLower();
    if (setting == "none")
        return 0;

    return syslogGetFacility(setting);
}

