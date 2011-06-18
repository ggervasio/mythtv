// POSIX headers
#include <fcntl.h> // for open flags

// C++ headers
#include <iostream>
#include <fstream>
#include <cerrno>
using namespace std;

// Qt headers
#include <QCoreApplication>
#include <QDir>

// MythTV headers
#include "exitcodes.h"
#include "programinfo.h"
#include "jobqueue.h"
#include "mythcontext.h"
#include "mythdb.h"
#include "mythverbose.h"
#include "mythversion.h"
#include "util.h"
#include "transcode.h"
#include "mpeg2fix.h"
#include "remotefile.h"
#include "mythtranslation.h"
#include "mythlogging.h"
#include "mythcommandlineparser.h"

static void CompleteJob(int jobID, ProgramInfo *pginfo, bool useCutlist,
                        frm_dir_map_t *deleteMap, int &resultCode);

static int glbl_jobID = -1;
static QString recorderOptions = "";

static void UpdatePositionMap(frm_pos_map_t &posMap, QString mapfile,
                       ProgramInfo *pginfo)
{
    if (pginfo && mapfile.isEmpty())
    {
        pginfo->ClearPositionMap(MARK_KEYFRAME);
        pginfo->ClearPositionMap(MARK_GOP_START);
        pginfo->SavePositionMap(posMap, MARK_GOP_BYFRAME);
    }
    else if (!mapfile.isEmpty())
    {
        FILE *mapfh = fopen(mapfile.toLocal8Bit().constData(), "w");
        if (!mapfh)
        {
            VERBOSE(VB_IMPORTANT,
                    QString("Could not open map file '%1'")
                    .arg(mapfile) + ENO);
            return;
        }
        frm_pos_map_t::const_iterator it;
        fprintf (mapfh, "Type: %d\n", MARK_GOP_BYFRAME);
        for (it = posMap.begin(); it != posMap.end(); ++it)
            fprintf(mapfh, "%lld %lld\n",
                    (unsigned long long)it.key(), (unsigned long long)*it);
        fclose(mapfh);
    }
}

static int BuildKeyframeIndex(MPEG2fixup *m2f, QString &infile,
                       frm_pos_map_t &posMap, int jobID)
{
    if (jobID < 0 || JobQueue::GetJobCmd(jobID) != JOB_STOP)
    {
        if (jobID >= 0)
            JobQueue::ChangeJobComment(jobID,
                QString(QObject::tr("Generating Keyframe Index")));
        int err = m2f->BuildKeyframeIndex(infile, posMap);
        if (err)
            return err;
        if (jobID >= 0)
            JobQueue::ChangeJobComment(jobID,
                QString(QObject::tr("Transcode Completed")));
    }
    return 0;
}

static void UpdateJobQueue(float percent_done)
{
    JobQueue::ChangeJobComment(glbl_jobID,
                               QString("%1% " + QObject::tr("Completed"))
                               .arg(percent_done, 0, 'f', 1));
}

static int CheckJobQueue()
{
    if (JobQueue::GetJobCmd(glbl_jobID) == JOB_STOP)
    {
        VERBOSE(VB_IMPORTANT, "Transcoding stopped by JobQueue");
        return 1;
    }
    return 0;
}

int main(int argc, char *argv[])
{
    uint chanid;
    QString starttime, infile, outfile;
    QString profilename = QString("autodetect");
    QString fifodir = NULL;
    int jobID = -1;
    QDateTime startts;
    int jobType = JOB_NONE;
    int otype = REPLEX_MPEG2;
    bool useCutlist = false, keyframesonly = false;
    bool build_index = false, fifosync = false, showprogress = false, mpeg2 = false;
    QMap<QString, QString> settingsOverride;
    frm_dir_map_t deleteMap;
    frm_pos_map_t posMap;
    srand(time(NULL));
    int AudioTrackNo = -1;

    QCoreApplication a(argc, argv);

    QCoreApplication::setApplicationName(MYTH_APPNAME_MYTHTRANSCODE);

    print_verbose_messages = VB_IMPORTANT;
    verboseString = "important";

    int found_starttime = 0;
    int found_chanid = 0;
    int found_infile = 0;
    int update_index = 1;
    int isVideo = 0;
    bool passthru = false;
    int quiet = 0;

    MythTranscodeCommandLineParser cmdline;
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

    if (cmdline.toBool("verbose"))
        if (parse_verbose_arg(cmdline.toString("verbose")) == 
                GENERIC_EXIT_INVALID_CMDLINE)
            return GENERIC_EXIT_INVALID_CMDLINE;
    if (cmdline.toBool("verboseint"))
        print_verbose_messages = cmdline.toUInt("verboseint");

    if (cmdline.toBool("starttime"))
    {
        starttime = cmdline.toString("starttime");
        found_starttime = 1;
    }
    if (cmdline.toBool("chanid"))
    {
        chanid = cmdline.toUInt("chanid");
        found_chanid = 1;
    }
    if (cmdline.toBool("jobid"))
        jobID = cmdline.toInt("jobid");
    if (cmdline.toBool("inputfile"))
    {
        infile = cmdline.toString("inputfile");
        found_infile = 1;
    }
    if (cmdline.toBool("outputfile"))
    {
        outfile = cmdline.toString("outputfile");
        update_index = 0;
    }
    if (cmdline.toBool("video"))
        isVideo = true;
    if (cmdline.toBool("profile"))
        profilename = cmdline.toString("profile");

    if (cmdline.toBool("usecutlist"))    
    {
        useCutlist = true;
        if (!cmdline.toString("usecutlist").isEmpty())
        {
            if (!cmdline.toBool("inputfile"))
            {
                cerr << "External cutlists are only allowed when using" << endl
                     << "the --infile option." << endl;
                return GENERIC_EXIT_INVALID_CMDLINE;
            }

            QStringList cutlist = cmdline.toStringList("usecutlist", " ");
            QStringList::iterator it;
            for (it = cutlist.begin(); it != cutlist.end(); ++it)
            {
                QStringList startend = (*it).split("-", QString::SkipEmptyParts);
                if (startend.size() == 2)
                {
                    cerr << "Cutting from: " << startend.first().toULongLong()
                         << " to: " << startend.last().toULongLong() << endl;
                    deleteMap[startend.first().toULongLong()] = MARK_CUT_START;
                    deleteMap[startend.last().toULongLong()] = MARK_CUT_END;
                }
            }
        }
    }

    if (cmdline.toBool("inversecut"))
    {
        if (!cmdline.toBool("inputfile"))
        {
            cerr << "Inversed cutlists are only allowed when using" << endl
                 << "the --infile option." << endl;
            return GENERIC_EXIT_INVALID_CMDLINE;
        }

        deleteMap[0] = MARK_CUT_START;

        uint64_t last = 0;
        QStringList cutlist = cmdline.toStringList("inversecut", " ");
        QStringList::iterator it;
        for (it = cutlist.begin(); it != cutlist.end(); ++it)
        {
            QStringList startend = (*it).split(
                        "-", QString::SkipEmptyParts);
            if (startend.count() == 2)
            {
                cerr << "Cutting from: " << last
                     << " to: "
                     << startend.first().toULongLong() <<"\n";
                deleteMap[startend.first().toULongLong()] = MARK_CUT_END;
                deleteMap[startend.last().toULongLong()] = MARK_CUT_START;
                last = startend.last().toInt();
            }
        }
        cerr << "Cutting from: " << last
             << " to the end\n";
        deleteMap[999999999] = MARK_CUT_END;
    }


    if (cmdline.toBool("allkeys"))
        keyframesonly = true;
    if (cmdline.toBool("reindex"))
        build_index = true;
    if (cmdline.toBool("fifodir"))
        fifodir = cmdline.toString("fifodir");
    if (cmdline.toBool("fifosync"))
        fifosync = true;
    if (cmdline.toBool("recopt"))
        recorderOptions = cmdline.toString("recopt");
    if (cmdline.toBool("showprogress"))
        showprogress = true;
    if (cmdline.toBool("mpeg2"))
        mpeg2 = true;
    if (cmdline.toBool("ostream"))
    {
        if (cmdline.toString("ostream") == "dvd")
            otype = REPLEX_DVD;
        else if (cmdline.toString("ostream") == "ts")
            otype = REPLEX_TS_SD;
        else
        {
            cerr << "Invalid 'ostream' type: "
                 << cmdline.toString("ostream").toLocal8Bit().constData()
                 << endl;
            return GENERIC_EXIT_INVALID_CMDLINE;
        }
    }
    if (cmdline.toBool("overridesettings"))
        settingsOverride = cmdline.GetSettingsOverride();
    if (cmdline.toBool("audiotrack"))
        AudioTrackNo = cmdline.toInt("audiotrack");
    if (cmdline.toBool("passthru"))
        passthru = true;

    if (outfile == "-")
        print_verbose_messages = VB_NONE;

    if (cmdline.toBool("quiet"))
    {
        quiet = cmdline.toUInt("quiet");
        if (quiet > 1)
        {
            print_verbose_messages = VB_NONE;
            parse_verbose_arg("none");
        }
    }

    int facility = cmdline.GetSyslogFacility();
    bool dblog = !cmdline.toBool("nodblog");

    QString logfile = cmdline.GetLogFilePath();
    logStart(logfile, quiet, facility, dblog);

    //  Load the context
    MythContext context(MYTH_BINARY_VERSION);
    gContext = &context;
    if (!gContext->Init(false))
    {
        VERBOSE(VB_IMPORTANT, "Failed to init MythContext, exiting.");
        return GENERIC_EXIT_NO_MYTHCONTEXT;
    }

    MythTranslation::load("mythfrontend");

    if (settingsOverride.size())
    {
        QMap<QString, QString>::iterator it;
        for (it = settingsOverride.begin(); it != settingsOverride.end(); ++it)
        {
            VERBOSE(VB_IMPORTANT, QString("Setting '%1' being forced to '%2'")
                                          .arg(it.key()).arg(*it));
            gCoreContext->OverrideSettingForSession(it.key(), *it);
        }
    }

    if (jobID != -1)
    {
        if (JobQueue::GetJobInfoFromID(
                jobID, jobType, chanid, startts))
        {
            starttime = startts.toString(Qt::ISODate);
            found_starttime = 1;
            found_chanid = 1;
        }
        else
        {
            cerr << "mythtranscode: ERROR: Unable to find DB info for "
                 << "JobQueue ID# " << jobID << endl;
            return GENERIC_EXIT_NO_RECORDING_DATA;
        }
    }

    if ((! found_infile && !(found_chanid && found_starttime)) ||
        (found_infile && (found_chanid || found_starttime)) )
    {
         cerr << "Must specify -i OR -c AND -s options!\n";
         return GENERIC_EXIT_INVALID_CMDLINE;
    }
    if (isVideo && !found_infile)
    {
         cerr << "Must specify --infile to use --video\n";
         return GENERIC_EXIT_INVALID_CMDLINE;
    }
    if (jobID >= 0 && (found_infile || build_index))
    {
         cerr << "Can't specify -j with --buildindex, --video or --infile\n";
         return GENERIC_EXIT_INVALID_CMDLINE;
    }
    if ((jobID >= 0) && build_index)
    {
         cerr << "Can't specify both -j and --buildindex\n";
         return GENERIC_EXIT_INVALID_CMDLINE;
    }
    if (keyframesonly && !fifodir.isEmpty())
    {
         cerr << "Cannot specify both --fifodir and --allkeys\n";
         return GENERIC_EXIT_INVALID_CMDLINE;
    }
    if (fifosync && fifodir.isEmpty())
    {
         cerr << "Must specify --fifodir to use --fifosync\n";
         return GENERIC_EXIT_INVALID_CMDLINE;
    }

    VERBOSE(VB_IMPORTANT, QString("Enabled verbose msgs: %1").arg(verboseString));

    if (!MSqlQuery::testDBConnection())
    {
        printf("couldn't open db\n");
        return GENERIC_EXIT_DB_ERROR;
    }

    ProgramInfo *pginfo = NULL;
    if (isVideo)
    {
        // We want the absolute file path for the filemarkup table
        QFileInfo inf(infile);
        infile = inf.absoluteFilePath();
        pginfo = new ProgramInfo(infile);
    }
    else if (!found_infile)
    {
        QDateTime recstartts = myth_dt_from_string(starttime);
        pginfo = new ProgramInfo(chanid, recstartts);

        if (!pginfo->GetChanID())
        {
            QString msg = QString("Couldn't find recording for chanid %1 @ %2")
                .arg(chanid).arg(starttime);
            cerr << msg.toLocal8Bit().constData() << endl;
            delete pginfo;
            return GENERIC_EXIT_NO_RECORDING_DATA;
        }

        infile = pginfo->GetPlaybackURL(false, true);
    }
    else
    {
        pginfo = new ProgramInfo(infile);
        if (!pginfo->GetChanID())
        {
            VERBOSE(VB_IMPORTANT,
                    QString("Couldn't find a recording for filename '%1'")
                    .arg(infile));
            delete pginfo;
            return GENERIC_EXIT_NO_RECORDING_DATA;
        }
    }

    if (!pginfo)
    {
        VERBOSE(VB_IMPORTANT, "No program info found!");
        return GENERIC_EXIT_NO_RECORDING_DATA;
    }

    if (infile.left(7) == "myth://" && (outfile.isNull() || outfile != "-"))
    {
        VERBOSE(VB_IMPORTANT, QString("Attempted to transcode %1. "
               "Mythtranscode is currently unable to transcode remote "
               "files.")
               .arg(infile));
        return GENERIC_EXIT_REMOTE_FILE;
    }

    if (outfile.isNull() && !build_index)
        outfile = infile + ".tmp";

    if (jobID >= 0)
        JobQueue::ChangeJobStatus(jobID, JOB_RUNNING);

    Transcode *transcode = new Transcode(pginfo);

    if (!build_index)
        VERBOSE(VB_GENERAL, QString("Transcoding from %1 to %2")
                            .arg(infile).arg(outfile));

    if (showprogress)
        transcode->ShowProgress(true);
    if (!recorderOptions.isEmpty())
        transcode->SetRecorderOptions(recorderOptions);
    int result = 0;
    if (!mpeg2 && !build_index)
    {
        result = transcode->TranscodeFile(infile, outfile,
                                          profilename, useCutlist,
                                          (fifosync || keyframesonly), jobID,
                                          fifodir, deleteMap, AudioTrackNo,
                                          passthru);
        if ((result == REENCODE_OK) && (jobID >= 0))
            JobQueue::ChangeJobArgs(jobID, "RENAME_TO_NUV");
    }

    int exitcode = GENERIC_EXIT_OK;
    if ((result == REENCODE_MPEG2TRANS) || mpeg2 || build_index)
    {
        void (*update_func)(float) = NULL;
        int (*check_func)() = NULL;
        if (useCutlist && !found_infile)
            pginfo->QueryCutList(deleteMap);
        if (jobID >= 0)
        {
           glbl_jobID = jobID;
           update_func = &UpdateJobQueue;
           check_func = &CheckJobQueue;
        }

        MPEG2fixup *m2f = new MPEG2fixup(
            infile, outfile,
            &deleteMap, NULL, false, false, 20,
            showprogress, otype, update_func,
            check_func);

        if (build_index)
        {
            int err = BuildKeyframeIndex(m2f, infile, posMap, jobID);
            if (err)
                return err;
            if (update_index)
                UpdatePositionMap(posMap, NULL, pginfo);
            else
                UpdatePositionMap(posMap, outfile + QString(".map"), pginfo);
        }
        else
        {
            result = m2f->Start();
            if (result == REENCODE_OK)
            {
                result = BuildKeyframeIndex(m2f, outfile, posMap, jobID);
                if (result == REENCODE_OK)
                {
                    if (update_index)
                        UpdatePositionMap(posMap, NULL, pginfo);
                    else
                        UpdatePositionMap(posMap, outfile + QString(".map"),
                                          pginfo);
                }
            }
        }
        delete m2f;
    }

    if (result == REENCODE_OK)
    {
        if (jobID >= 0)
            JobQueue::ChangeJobStatus(jobID, JOB_STOPPING);
        VERBOSE(VB_GENERAL, QString("%1 %2 done")
                .arg(build_index ? "Building Index for" : "Transcoding")
                .arg(infile));
    }
    else if (result == REENCODE_CUTLIST_CHANGE)
    {
        if (jobID >= 0)
            JobQueue::ChangeJobStatus(jobID, JOB_RETRY);
        VERBOSE(VB_GENERAL, QString("Transcoding %1 aborted because of "
                                    "cutlist update").arg(infile));
        exitcode = GENERIC_EXIT_RESTART;
    }
    else if (result == REENCODE_STOPPED)
    {
        if (jobID >= 0)
            JobQueue::ChangeJobStatus(jobID, JOB_ABORTING);
        VERBOSE(VB_GENERAL, QString("Transcoding %1 stopped because of "
                                    "stop command").arg(infile));
        exitcode = GENERIC_EXIT_KILLED;
    }
    else
    {
        if (jobID >= 0)
            JobQueue::ChangeJobStatus(jobID, JOB_ERRORING);
        VERBOSE(VB_GENERAL, QString("Transcoding %1 failed").arg(infile));
        exitcode = result;
    }

    if (jobID >= 0)
        CompleteJob(jobID, pginfo, useCutlist, &deleteMap, exitcode);

    transcode->deleteLater();

    return exitcode;
}

static int transUnlink(QString filename, ProgramInfo *pginfo)
{
    if (pginfo != NULL && !pginfo->GetStorageGroup().isEmpty() &&
        !pginfo->GetHostname().isEmpty())
    {
        QString ip = gCoreContext->GetSettingOnHost("BackendServerIP",
                                                pginfo->GetHostname());
        QString port = gCoreContext->GetSettingOnHost("BackendServerPort",
                                                  pginfo->GetHostname());
        QString basename = filename.section('/', -1);
        QString uri = gCoreContext->GenMythURL(ip,port,basename,pginfo->GetStorageGroup());

        VERBOSE(VB_IMPORTANT, QString("Requesting delete for file '%1'.")
                                      .arg(uri));
        bool ok = RemoteFile::DeleteFile(uri);
        if (ok)
            return 0;
    }

    VERBOSE(VB_IMPORTANT, QString("Deleting file '%1'.").arg(filename));
    return unlink(filename.toLocal8Bit().constData());
}

static uint64_t ComputeNewBookmark(uint64_t oldBookmark,
                            frm_dir_map_t *deleteMap)
{
    if (deleteMap == NULL)
        return oldBookmark;
    uint64_t subtraction = 0;
    uint64_t startOfCutRegion = 0;
    frm_dir_map_t delMap = *deleteMap;
    bool withinCut = false;
    bool firstMark = true;
    while (delMap.count() && delMap.begin().key() <= oldBookmark)
    {
        if (delMap.begin().data() == MARK_CUT_START && !withinCut)
        {
            withinCut = true;
            startOfCutRegion = delMap.begin().key();
        }
        else if (delMap.begin().data() == MARK_CUT_END && firstMark)
        {
            subtraction += delMap.begin().key();
        }
        else if (delMap.begin().data() == MARK_CUT_END && withinCut)
        {
            withinCut = false;
            subtraction += (delMap.begin().key() - startOfCutRegion);
        }
        delMap.remove(delMap.begin());
        firstMark = false;
    }
    if (withinCut)
        subtraction += (oldBookmark - startOfCutRegion);
    return oldBookmark - subtraction;
}

static uint64_t ReloadBookmark(ProgramInfo *pginfo)
{
    MSqlQuery query(MSqlQuery::InitCon());
    uint64_t currentBookmark = 0;
    query.prepare("SELECT DISTINCT mark FROM recordedmarkup "
                  "WHERE chanid = :CHANID "
                  "AND starttime = :STARTIME "
                  "AND type = :MARKTYPE ;");
    query.bindValue(":CHANID", pginfo->GetChanID());
    query.bindValue(":STARTTIME", pginfo->GetRecordingStartTime());
    query.bindValue(":MARKTYPE", MARK_BOOKMARK);
    if (query.exec() && query.next())
    {
        currentBookmark = query.value(0).toLongLong();
    }
    return currentBookmark;
}

static void WaitToDelete(ProgramInfo *pginfo)
{
    VERBOSE(VB_GENERAL,
            "Transcode: delete old file: "
            "waiting while program is in use.");
    bool inUse = true;
    MSqlQuery query(MSqlQuery::InitCon());
    while (inUse)
    {
        query.prepare("SELECT count(*) FROM inuseprograms "
                      "WHERE chanid = :CHANID "
                      "AND starttime = :STARTTIME "
                      "AND recusage = 'player' ;");
        query.bindValue(":CHANID", pginfo->GetChanID());
        query.bindValue(":STARTTIME", pginfo->GetRecordingStartTime());
        if (!query.exec() || !query.next())
        {
            VERBOSE(VB_GENERAL,
                    "Transcode: delete old file: in-use query failed;");
            inUse = false;
        }
        else
        {
            inUse = (query.value(0).toUInt() != 0);
        }

        if (inUse)
        {
            const unsigned kSecondsToWait = 10;
            VERBOSE(VB_GENERAL,
                    QString("Transcode: program in use, "
                            "rechecking in %1 seconds.").arg(kSecondsToWait));
            sleep(kSecondsToWait);
        }
    }
    VERBOSE(VB_GENERAL, "Transcode: program is no longer in use.");
}

static void CompleteJob(int jobID, ProgramInfo *pginfo, bool useCutlist,
                 frm_dir_map_t *deleteMap, int &resultCode)
{
    int status = JobQueue::GetJobStatus(jobID);

    if (!pginfo)
    {
        JobQueue::ChangeJobStatus(jobID, JOB_ERRORED,
                  "Job errored, unable to find Program Info for job");
        return;
    }

    WaitToDelete(pginfo);

    const QString filename = pginfo->GetPlaybackURL(false, true);
    const QByteArray fname = filename.toLocal8Bit();

    if (status == JOB_STOPPING)
    {
        // Transcoding may take several minutes.  Reload the bookmark
        // in case it changed, then save its translated value back.
        uint64_t previousBookmark =
            ComputeNewBookmark(ReloadBookmark(pginfo), deleteMap);
        pginfo->SaveBookmark(previousBookmark);

        const QString jobArgs = JobQueue::GetJobArgs(jobID);

        const QString tmpfile = filename + ".tmp";
        const QByteArray atmpfile = tmpfile.toLocal8Bit();

        // To save the original file...
        const QString oldfile = filename + ".old";
        const QByteArray aoldfile = oldfile.toLocal8Bit();

        QFileInfo st(tmpfile);
        qint64 newSize = 0;
        if (st.exists()) 
            newSize = st.size();

        QString cnf = filename;
        if ((jobArgs == "RENAME_TO_NUV") &&
            (filename.contains(QRegExp("mpg$"))))
        {
            QString newbase = pginfo->QueryBasename();

            cnf.replace(QRegExp("mpg$"), "nuv");
            newbase.replace(QRegExp("mpg$"), "nuv");
            pginfo->SaveBasename(newbase);
        }

        const QString newfile = cnf;
        const QByteArray anewfile = newfile.toLocal8Bit();

        if (rename(fname.constData(), aoldfile.constData()) == -1)
        {
            VERBOSE(VB_IMPORTANT,
                    QString("mythtranscode: Error Renaming '%1' to '%2'")
                    .arg(filename).arg(oldfile) + ENO);
        }

        if (rename(atmpfile.constData(), anewfile.constData()) == -1)
        {
            VERBOSE(VB_IMPORTANT,
                    QString("mythtranscode: Error Renaming '%1' to '%2'")
                    .arg(tmpfile).arg(newfile) + ENO);
        }

        if (!gCoreContext->GetNumSetting("SaveTranscoding", 0))
        {
            int err;
            bool followLinks = gCoreContext->GetNumSetting("DeletesFollowLinks", 0);

            VERBOSE(VB_FILE, QString("mythtranscode: About to unlink/delete "
                                     "file: %1").arg(oldfile));
            QFileInfo finfo(oldfile);
            if (followLinks && finfo.isSymLink())
            {
                QString link = getSymlinkTarget(oldfile);
                QByteArray alink = link.toLocal8Bit();
                err = transUnlink(alink.constData(), pginfo);

                if (err)
                {
                    VERBOSE(VB_IMPORTANT, QString(
                                "mythtranscode: Error deleting '%1' "
                                "pointed to by '%2'")
                            .arg(alink.constData())
                            .arg(aoldfile.constData()) + ENO);
                }

                err = unlink(aoldfile.constData());
                if (err)
                {
                    VERBOSE(VB_IMPORTANT, QString(
                                "mythtranscode: Error deleting '%1', "
                                "a link pointing to '%2'")
                            .arg(aoldfile.constData())
                            .arg(alink.constData()) + ENO);
                }
            }
            else
            {
                if ((err = transUnlink(aoldfile.constData(), pginfo)))
                    VERBOSE(VB_IMPORTANT, QString(
                            "mythtranscode: Error deleting '%1', %2")
                            .arg(oldfile).arg(strerror(errno)));
            }
        }

        // Delete previews if cutlist was applied.  They will be re-created as
        // required.  This prevents the user from being stuck with a preview
        // from a cut area and ensures that the "dimensioned" previews
        // correspond to the new timeline
        if (useCutlist)
        {
            QFileInfo fInfo(filename);
            QString nameFilter = fInfo.fileName() + "*.png";
            // QDir's nameFilter uses spaces or semicolons to separate globs,
            // so replace them with the "match any character" wildcard
            // since mythrename.pl may have included them in filenames
            nameFilter.replace(QRegExp("( |;)"), "?");
            QDir dir (fInfo.path(), nameFilter);

            for (uint nIdx = 0; nIdx < dir.count(); nIdx++)
            {
                // If unlink fails, keeping the old preview is not a problem.
                // The RENAME_TO_NUV check below will attempt to rename the
                // file, if required.
                const QString oldfileop = QString("%1/%2")
                    .arg(fInfo.path()).arg(dir[nIdx]);
                const QByteArray aoldfileop = oldfileop.toLocal8Bit();
                transUnlink(aoldfileop.constData(), pginfo);
            }
        }

        /* Rename all preview thumbnails. */
        if (jobArgs == "RENAME_TO_NUV")
        {
            QFileInfo fInfo(filename);
            QString nameFilter = fInfo.fileName() + "*.png";
            // QDir's nameFilter uses spaces or semicolons to separate globs,
            // so replace them with the "match any character" wildcard
            // since mythrename.pl may have included them in filenames
            nameFilter.replace(QRegExp("( |;)"), "?");

            QDir dir (fInfo.path(), nameFilter);

            for (uint nIdx = 0; nIdx < dir.count(); nIdx++)
            {
                const QString oldfileprev = QString("%1/%2")
                    .arg(fInfo.path()).arg(dir[nIdx]);
                const QByteArray aoldfileprev = oldfileprev.toLocal8Bit();

                QString newfileprev = oldfileprev;
                QRegExp re("mpg(\\..*)?\\.png$");
                if (re.indexIn(newfileprev))
                {
                    newfileprev.replace(
                        re, QString("nuv%1.png").arg(re.cap(1)));
                }
                const QByteArray anewfileprev = newfileprev.toLocal8Bit();

                QFile checkFile(oldfileprev);

                if ((oldfileprev != newfileprev) && (checkFile.exists()))
                    rename(aoldfileprev.constData(), anewfileprev.constData());
            }
        }

        MSqlQuery query(MSqlQuery::InitCon());

        if (useCutlist)
        {
            query.prepare("DELETE FROM recordedmarkup "
                          "WHERE chanid = :CHANID "
                          "AND starttime = :STARTTIME "
                          "AND type != :BOOKMARK ");
            query.bindValue(":CHANID", pginfo->GetChanID());
            query.bindValue(":STARTTIME", pginfo->GetRecordingStartTime());
            query.bindValue(":BOOKMARK", MARK_BOOKMARK);

            if (!query.exec())
                MythDB::DBError("Error in mythtranscode", query);

            query.prepare("UPDATE recorded "
                          "SET cutlist = :CUTLIST "
                          "WHERE chanid = :CHANID "
                          "AND starttime = :STARTTIME ;");
            query.bindValue(":CUTLIST", "0");
            query.bindValue(":CHANID", pginfo->GetChanID());
            query.bindValue(":STARTTIME", pginfo->GetRecordingStartTime());

            if (!query.exec())
                MythDB::DBError("Error in mythtranscode", query);

            pginfo->SaveCommFlagged(COMM_FLAG_NOT_FLAGGED);
        }
        else
        {
            query.prepare("DELETE FROM recordedmarkup "
                          "WHERE chanid = :CHANID "
                          "AND starttime = :STARTTIME "
                          "AND type not in ( :COMM_START, "
                          "    :COMM_END, :BOOKMARK, "
                          "    :CUTLIST_START, :CUTLIST_END) ;");
            query.bindValue(":CHANID", pginfo->GetChanID());
            query.bindValue(":STARTTIME", pginfo->GetRecordingStartTime());
            query.bindValue(":COMM_START", MARK_COMM_START);
            query.bindValue(":COMM_END", MARK_COMM_END);
            query.bindValue(":BOOKMARK", MARK_BOOKMARK);
            query.bindValue(":CUTLIST_START", MARK_CUT_START);
            query.bindValue(":CUTLIST_END", MARK_CUT_END);

            if (!query.exec())
                MythDB::DBError("Error in mythtranscode", query);
        }

        if (newSize)
            pginfo->SaveFilesize(newSize);

        JobQueue::ChangeJobStatus(jobID, JOB_FINISHED);

    } else {
        // Not a successful run, so remove the files we created
        QString filename_tmp = filename + ".tmp";
        QByteArray fname_tmp = filename_tmp.toLocal8Bit();
        VERBOSE(VB_IMPORTANT, QString("Deleting %1").arg(filename_tmp));
        transUnlink(fname_tmp.constData(), pginfo);

        QString filename_map = filename + ".tmp.map";
        QByteArray fname_map = filename_map.toLocal8Bit();
        unlink(fname_map.constData());

        if (status == JOB_ABORTING)                     // Stop command was sent
            JobQueue::ChangeJobStatus(jobID, JOB_ABORTED, "Job Aborted");
        else if (status != JOB_ERRORING)                // Recoverable error
            resultCode = GENERIC_EXIT_RESTART;
        else                                            // Unrecoverable error
            JobQueue::ChangeJobStatus(jobID, JOB_ERRORED,
                                      "Unrecoverable error");
    }
}
/* vim: set expandtab tabstop=4 shiftwidth=4: */
