/*
    main.cpp

    Starting point for the myth lcd server daemon

*/

#include <iostream>
using namespace std;
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>

#include <QCoreApplication>
#include <QFile>

#include "exitcodes.h"
#include "mythcontext.h"
#include "mythdbcon.h"
#include "mythlogging.h"
#include "mythversion.h"
#include "tv_play.h"
#include "compat.h"
#include "mythtranslation.h"
#include "mythcommandlineparser.h"

#include "lcdserver.h"

int main(int argc, char **argv)
{
    QCoreApplication a(argc, argv);
    bool daemon_mode = false;
    int  special_port = -1;
    QString startup_message = "";          // default to no startup message
    int message_time = 30;                 // time to display startup message
    verboseMask = VB_IMPORTANT;            // only show important messages
    QString logfile;
    int quiet = 0;

    debug_level = 0;  // don't show any debug messages by default

    QCoreApplication::setApplicationName(MYTH_APPNAME_MYTHLCDSERVER);

    MythLCDServerCommandLineParser cmdline;
    if (!cmdline.Parse(argc, argv))
    {
        cmdline.PrintHelp();
        return GENERIC_EXIT_INVALID_CMDLINE;
    }

    if (cmdline.toBool("showhelp"))
    {
        cmdline.PrintHelp();
        return GENERIC_EXIT_OK;
    }

    if (cmdline.toBool("showversion"))
    {
        cmdline.PrintVersion();
        return GENERIC_EXIT_OK;
    }

    if (cmdline.toBool("quiet"))
    {
        quiet = cmdline.toUInt("quiet");
        if (quiet > 1)
        {
            verboseMask = VB_NONE;
            verboseArgParse("none");
        }
    }

    if (cmdline.toBool("daemon"))
    {
        daemon_mode = true;
        if (!quiet)
            quiet = 1;
    }

    int facility = cmdline.GetSyslogFacility();
    bool dblog = !cmdline.toBool("nodblog");
    LogLevel_t level = cmdline.GetLogLevel();
    if (level == LOG_UNKNOWN)
        return GENERIC_EXIT_INVALID_CMDLINE;

    logfile = cmdline.GetLogFilePath();
    bool propagate = cmdline.toBool("islogpath");
    logStart(logfile, quiet, facility, level, dblog, propagate);


    if (cmdline.toBool("port"))
    {
        special_port = cmdline.toInt("port");
        if (special_port < 1 || special_port > 65534)
        {
            VERBOSE(VB_IMPORTANT, "lcdserver: Bad port number");
            return GENERIC_EXIT_INVALID_CMDLINE;
        }
    }
    if (cmdline.toBool("message"))
        startup_message = cmdline.toString("message");
    if (cmdline.toBool("messagetime"))
    {
        message_time = cmdline.toInt("messagetime");
        if (message_time < 1 || message_time > 1000)
        {
            VERBOSE(VB_IMPORTANT, "lcdserver: Bad message duration");
            return GENERIC_EXIT_INVALID_CMDLINE;
        }
    }
    if (cmdline.toBool("debug"))
    {
        debug_level = cmdline.toInt("debug");
        if (debug_level < 0 || debug_level > 10)
        {
            VERBOSE(VB_IMPORTANT, "lcdserver: Bad debug level");
            return GENERIC_EXIT_INVALID_CMDLINE;
        }
    }

    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR)
        cerr << "Unable to ignore SIGPIPE\n";

    //  Switch to daemon mode?
    if (daemon_mode)
    {
        if (daemon(0, 1) < 0)
        {
            VERBOSE(VB_IMPORTANT, "lcdserver: Failed to run as a daemon. "
                            "Bailing out.");
            return GENERIC_EXIT_DAEMONIZING_ERROR;
        }
        cout << endl;
    }

    //  Get the MythTV context and db hooks
    gContext = new MythContext(MYTH_BINARY_VERSION);
    if (!gContext->Init(false))
    {
        VERBOSE(VB_IMPORTANT, "lcdserver: Could not initialize MythContext. "
                        "Exiting.");
        return GENERIC_EXIT_NO_MYTHCONTEXT;
    }

    MythTranslation::load("mythfrontend");

    gCoreContext->ConnectToMasterServer(false);

    //  Figure out port to listen on
    int assigned_port = gCoreContext->GetNumSetting("LCDServerPort", 6545);
    if (special_port > 0)
    {
        assigned_port = special_port;
    }

    new LCDServer(assigned_port, startup_message, message_time);

    a.exec();

    delete gContext;
    return GENERIC_EXIT_OK;
}

