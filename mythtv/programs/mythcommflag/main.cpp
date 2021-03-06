// POSIX headers
#include <unistd.h>
#include <sys/time.h> // for gettimeofday

// ANSI C headers
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <cmath>

// C++ headers
#include <string>
#include <iostream>
#include <fstream>
using namespace std;

// Qt headers
#include <QCoreApplication>
#include <QString>
#include <QRegExp>
#include <QDir>
#include <QEvent>

// MythTV headers
#include "util.h"
#include "exitcodes.h"
#include "mythcontext.h"
#include "mythdb.h"
#include "mythversion.h"
#include "mythcommflagplayer.h"
#include "programinfo.h"
#include "remoteutil.h"
#include "remotefile.h"
#include "tvremoteutil.h"
#include "jobqueue.h"
#include "remoteencoder.h"
#include "ringbuffer.h"
#include "commandlineparser.h"
#include "mythtranslation.h"
#include "mythlogging.h"

// Commercial Flagging headers
#include "CommDetectorBase.h"
#include "CommDetectorFactory.h"
#include "SlotRelayer.h"
#include "CustomEventRelayer.h"

#define LOC      QString("MythCommFlag: ")
#define LOC_WARN QString("MythCommFlag, Warning: ")
#define LOC_ERR  QString("MythCommFlag, Error: ")

namespace
{
    void cleanup()
    {
        delete gContext;
        gContext = NULL;

    }

    class CleanupGuard
    {
      public:
        typedef void (*CleanupFunc)();

      public:
        CleanupGuard(CleanupFunc cleanFunction) :
            m_cleanFunction(cleanFunction) {}

        ~CleanupGuard()
        {
            m_cleanFunction();
        }

      private:
        CleanupFunc m_cleanFunction;
    };
}

int  quiet = 0;
bool progress = true;
bool force = false;

MythCommFlagCommandLineParser cmdline;

bool fullSpeed = true;
bool rebuildSeekTable = false;
bool beNice = true;
bool inJobQueue = false;
bool watchingRecording = false;
CommDetectorBase* commDetector = NULL;
RemoteEncoder* recorder = NULL;
ProgramInfo *global_program_info = NULL;
enum SkipTypes commDetectMethod = COMM_DETECT_UNINIT;
int recorderNum = -1;
bool dontSubmitCommbreakListToDB =  false;
bool onlyDumpDBCommercialBreakList = false;

int jobID = -1;
int lastCmd = -1;

static QMap<QString,SkipTypes> *init_skip_types();
QMap<QString,SkipTypes> *skipTypes = init_skip_types();

static QMap<QString,SkipTypes> *init_skip_types(void)
{
    QMap<QString,SkipTypes> *tmp = new QMap<QString,SkipTypes>;
    (*tmp)["commfree"]    = COMM_DETECT_COMMFREE;
    (*tmp)["uninit"]      = COMM_DETECT_UNINIT;
    (*tmp)["off"]         = COMM_DETECT_OFF;
    (*tmp)["blank"]       = COMM_DETECT_BLANKS;
    (*tmp)["blanks"]      = COMM_DETECT_BLANKS;
    (*tmp)["scene"]       = COMM_DETECT_SCENE;
    (*tmp)["blankscene"]  = COMM_DETECT_BLANK_SCENE;
    (*tmp)["blank_scene"] = COMM_DETECT_BLANK_SCENE;
    (*tmp)["logo"]        = COMM_DETECT_LOGO;
    (*tmp)["all"]         = COMM_DETECT_ALL;
    (*tmp)["d2"]          = COMM_DETECT_2;
    (*tmp)["d2_logo"]     = COMM_DETECT_2_LOGO;
    (*tmp)["d2_blank"]    = COMM_DETECT_2_BLANK;
    (*tmp)["d2_scene"]    = COMM_DETECT_2_SCENE;
    (*tmp)["d2_all"]      = COMM_DETECT_2_ALL;
    return tmp;
}

typedef enum
{
    kOutputMethodEssentials = 1,
    kOutputMethodFull,
} OutputMethod;
OutputMethod outputMethod = kOutputMethodEssentials;

static QMap<QString,OutputMethod> *init_output_types();
QMap<QString,OutputMethod> *outputTypes = init_output_types();

static QMap<QString,OutputMethod> *init_output_types(void)
{
    QMap<QString,OutputMethod> *tmp = new QMap<QString,OutputMethod>;
    (*tmp)["essentials"] = kOutputMethodEssentials;
    (*tmp)["full"]       = kOutputMethodFull;
    return tmp;
}

static QString get_filename(ProgramInfo *program_info)
{
    QString filename = program_info->GetPathname();
    if (!QFile::exists(filename))
        filename = program_info->GetPlaybackURL(true);
    return filename;
}

static int BuildVideoMarkup(ProgramInfo *program_info, bool useDB)
{
    QString filename;

    if (program_info->IsMythStream())
        filename = program_info->GetPathname();
    else
        filename = get_filename(program_info);

    RingBuffer *tmprbuf = RingBuffer::Create(filename, false);
    if (!tmprbuf)
    {
        LOG(VB_GENERAL, LOG_ERR,
            QString("Unable to create RingBuffer for %1").arg(filename));
        return GENERIC_EXIT_PERMISSIONS_ERROR;
    }

    if (useDB && !MSqlQuery::testDBConnection())
    {
        LOG(VB_GENERAL, LOG_ERR,
            "Unable to open DB connection for commercial flagging.");
        delete tmprbuf;
        return GENERIC_EXIT_DB_ERROR;
    }

    MythCommFlagPlayer *cfp = new MythCommFlagPlayer();
    PlayerContext *ctx = new PlayerContext("seektable rebuilder");
    ctx->SetSpecialDecode(kAVSpecialDecode_NoDecode);
    ctx->SetPlayingInfo(program_info);
    ctx->SetRingBuffer(tmprbuf);
    ctx->SetPlayer(cfp);
    cfp->SetPlayerInfo(NULL, NULL, true, ctx);
    cfp->RebuildSeekTable(progress);

    if (progress)
        cerr << "Rebuilt" << endl;

    delete ctx;

    return GENERIC_EXIT_OK;
}

static int QueueCommFlagJob(uint chanid, QDateTime starttime, bool rebuild)
{
    QString startstring = starttime.toString("yyyyMMddhhmmss");
    const ProgramInfo pginfo(chanid, starttime);

    if (!pginfo.GetChanID())
    {
        if (progress)
        {
            QString tmp = QString(
                "Unable to find program info for chanid %1 @ %2")
                .arg(chanid).arg(startstring);
            cerr << tmp.toLocal8Bit().constData() << endl;
        }
        return GENERIC_EXIT_NO_RECORDING_DATA;
    }

    bool result = JobQueue::QueueJob(JOB_COMMFLAG,
        pginfo.GetChanID(), pginfo.GetRecordingStartTime(), "", "", "",
        rebuild ? JOB_REBUILD : 0, JOB_QUEUED, QDateTime());

    if (result)
    {
        if (progress)
        {
            QString tmp = QString("Job Queued for chanid %1 @ %2")
                .arg(chanid).arg(startstring);
            cerr << tmp.toLocal8Bit().constData() << endl;
        }
        return GENERIC_EXIT_OK;
    }
    else
    {
        if (progress)
        {
            QString tmp = QString("Error queueing job for chanid %1 @ %2")
                .arg(chanid).arg(startstring);
            cerr << tmp.toLocal8Bit().constData() << endl;
        }
        return GENERIC_EXIT_DB_ERROR;
    }

    return GENERIC_EXIT_OK;
}

static int CopySkipListToCutList(uint chanid, QDateTime starttime)
{
    frm_dir_map_t cutlist;
    frm_dir_map_t::const_iterator it;

    QString startstring = starttime.toString("yyyyMMddhhmmss");
    const ProgramInfo pginfo(chanid, starttime);

    if (!pginfo.GetChanID())
    {
        LOG(VB_GENERAL, LOG_ERR,
            QString("No program data exists for channel %1 at %2")
                .arg(chanid).arg(startstring));
        return GENERIC_EXIT_NO_RECORDING_DATA;
    }

    pginfo.QueryCommBreakList(cutlist);
    for (it = cutlist.begin(); it != cutlist.end(); ++it)
        if (*it == MARK_COMM_START)
            cutlist[it.key()] = MARK_CUT_START;
        else
            cutlist[it.key()] = MARK_CUT_END;
    pginfo.SaveCutList(cutlist);

    return GENERIC_EXIT_OK;
}

static int ClearSkipList(uint chanid, QDateTime starttime)
{
    QString startstring = starttime.toString("yyyyMMddhhmmss");
    const ProgramInfo pginfo(chanid, starttime);

    if (!pginfo.GetChanID())
    {
        LOG(VB_GENERAL, LOG_ERR,
            QString("No program data exists for channel %1 at %2")
                .arg(chanid).arg(startstring));
        return GENERIC_EXIT_NO_RECORDING_DATA;
    }

    frm_dir_map_t skiplist;
    pginfo.SaveCommBreakList(skiplist);

    LOG(VB_GENERAL, LOG_NOTICE, "Commercial skip list cleared");

    return GENERIC_EXIT_OK;
}

static int SetCutList(uint chanid, QDateTime starttime, QString newCutList)
{
    frm_dir_map_t cutlist;

    newCutList.replace(QRegExp(" "), "");

    QStringList tokens = newCutList.split(",", QString::SkipEmptyParts);

    for (int i = 0; i < tokens.size(); i++)
    {
        QStringList cutpair = tokens[i].split("-", QString::SkipEmptyParts);
        cutlist[cutpair[0].toInt()] = MARK_CUT_START;
        cutlist[cutpair[1].toInt()] = MARK_CUT_END;
    }

    QString startstring = starttime.toString("yyyyMMddhhmmss");
    const ProgramInfo pginfo(chanid, starttime);

    if (!pginfo.GetChanID())
    {
        LOG(VB_GENERAL, LOG_ERR,
            QString("No program data exists for channel %1 at %2")
                .arg(chanid).arg(startstring));
        return GENERIC_EXIT_NO_RECORDING_DATA;
    }

    pginfo.SaveCutList(cutlist);

    LOG(VB_GENERAL, LOG_NOTICE, QString("Cutlist set to: %1").arg(newCutList));

    return GENERIC_EXIT_OK;
}

static int GetMarkupList(QString list, uint chanid, QDateTime starttime)
{
    frm_dir_map_t cutlist;
    frm_dir_map_t::const_iterator it;
    QString result;

    QString startstring = starttime.toString("yyyyMMddhhmmss");
    const ProgramInfo pginfo(chanid, starttime);

    if (!pginfo.GetChanID())
    {
        LOG(VB_GENERAL, LOG_ERR,
            QString("No program data exists for channel %1 at %2")
                .arg(chanid).arg(startstring));
        return GENERIC_EXIT_NO_RECORDING_DATA;
    }

    if (list == "cutlist")
        pginfo.QueryCutList(cutlist);
    else
        pginfo.QueryCommBreakList(cutlist);

    uint64_t lastStart = 0;
    for (it = cutlist.begin(); it != cutlist.end(); ++it)
    {
        if ((*it == MARK_COMM_START) ||
            (*it == MARK_CUT_START))
        {
            if (!result.isEmpty())
                result += ",";
            lastStart = it.key();
            result += QString("%1-").arg(lastStart);
        }
        else
        {
            if (result.isEmpty())
                result += "0-";
            result += QString("%1").arg(it.key());
        }
    }

    if (result.endsWith('-'))
    {
        uint64_t lastFrame = pginfo.QueryLastFrameInPosMap() + 60;
        if (lastFrame > lastStart)
            result += QString("%1").arg(lastFrame);
    }

    if (list == "cutlist")
        cout << QString("Cutlist: %1\n").arg(result).toLocal8Bit().constData();
    else
    {
        cout << QString("Commercial Skip List: %1\n")
            .arg(result).toLocal8Bit().constData();
    }

    return GENERIC_EXIT_OK;
}

static void streamOutCommercialBreakList(
    ostream &output, const frm_dir_map_t &commercialBreakList)
{
    if (progress)
        output << "----------------------------" << endl;

    if (commercialBreakList.empty())
    {
        if (progress)
            output << "No breaks" << endl;
    }
    else
    {
        frm_dir_map_t::const_iterator it = commercialBreakList.begin();
        for (; it != commercialBreakList.end(); it++)
        {
            output << "framenum: " << it.key() << "\tmarktype: " << *it
                   << endl;
        }
    }

    if (progress)
        output << "----------------------------" << endl;
}

static void print_comm_flag_output(
    const ProgramInfo          *program_info,
    const frm_dir_map_t        &commBreakList,
    uint64_t                    frame_count,
    const CommDetectorBase     *commDetect,
    const QString              &output_filename)
{
    if (output_filename.isEmpty())
        return;

    ostream *out = &cout;
    if (output_filename != "-")
    {
        QByteArray tmp = output_filename.toLocal8Bit();
        out = new fstream(tmp.constData(), ios::app | ios::out );
    }

    if (progress)
    {
        QString tmp = "";
        if (program_info->GetChanID())
        {
            tmp = QString("commercialBreakListFor: %1 on %2 @ %3")
                .arg(program_info->GetTitle())
                .arg(program_info->GetChanID())
                .arg(program_info->GetRecordingStartTime(ISODate));
        }
        else
        {
            tmp = QString("commercialBreakListFor: %1")
                .arg(program_info->GetPathname());
        }

        const QByteArray tmp2 = tmp.toLocal8Bit();
        *out << tmp2.constData() << endl;

        if (frame_count)
            *out << "totalframecount: " << frame_count << endl;
    }

    if (commDetect)
        commDetect->PrintFullMap(*out, &commBreakList, progress);
    else
        streamOutCommercialBreakList(*out, commBreakList);

    if (output_filename != "-")
        delete out;
}

static void commDetectorBreathe()
{
    //this is connected to the commdetectors breathe signal so we get a chance
    //while its busy to see if the user already told us to stop.
    qApp->processEvents();

    if (jobID != -1)
    {
        int curCmd = JobQueue::GetJobCmd(jobID);
        if (curCmd == lastCmd)
            return;

        switch (curCmd)
        {
                case JOB_STOP:
                {
                    commDetector->stop();
                    break;
                }
                case JOB_PAUSE:
                {
                    JobQueue::ChangeJobStatus(jobID, JOB_PAUSED,
                                              QObject::tr("Paused"));
                    commDetector->pause();
                    break;
                }
                case JOB_RESUME:
                {
                    JobQueue::ChangeJobStatus(jobID, JOB_RUNNING,
                                              QObject::tr("Running"));
                    commDetector->resume();
                    break;
                }
        }
    }
}

static void commDetectorStatusUpdate(const QString& status)
{
    if (jobID != -1)
    {
        JobQueue::ChangeJobStatus(jobID, JOB_RUNNING,  status);
        JobQueue::ChangeJobComment(jobID,  status);
    }
}

static void commDetectorGotNewCommercialBreakList(void)
{
    frm_dir_map_t newCommercialMap;
    commDetector->GetCommercialBreakList(newCommercialMap);

    frm_dir_map_t::Iterator it = newCommercialMap.begin();
    QString message = "COMMFLAG_UPDATE ";
    message += global_program_info->MakeUniqueKey();

    for (it = newCommercialMap.begin();
            it != newCommercialMap.end(); ++it)
    {
        if (it != newCommercialMap.begin())
            message += ",";
        else
            message += " ";
        message += QString("%1:%2").arg(it.key())
                   .arg(*it);
    }

    LOG(VB_COMMFLAG, LOG_INFO,
        QString("mythcommflag sending update: %1").arg(message));

    RemoteSendMessage(message);
}

static void incomingCustomEvent(QEvent* e)
{
    if ((MythEvent::Type)(e->type()) == MythEvent::MythEventMessage)
    {
        MythEvent *me = (MythEvent *)e;
        QString message = me->Message();

        message = message.simplified();
        QStringList tokens = message.split(" ", QString::SkipEmptyParts);

        LOG(VB_COMMFLAG, LOG_INFO,
            QString("mythcommflag: Received Event: '%1'") .arg(message));

        if ((watchingRecording) && (tokens.size() >= 3) &&
            (tokens[0] == "DONE_RECORDING"))
        {
            int cardnum = tokens[1].toInt();
            int filelen = tokens[2].toInt();

            message = QString("mythcommflag: Received a "
                              "DONE_RECORDING event for card %1.  ")
                              .arg(cardnum);

            if (recorderNum != -1 && cardnum == recorderNum)
            {
                commDetector->recordingFinished(filelen);
                watchingRecording = false;
                message += "Informed CommDetector that recording has finished.";
                LOG(VB_COMMFLAG, LOG_INFO, message);
            }
        }

        if ((tokens.size() >= 2) && (tokens[0] == "COMMFLAG_REQUEST"))
        {
            uint chanid = 0;
            QDateTime recstartts;
            ProgramInfo::ExtractKey(tokens[1], chanid, recstartts);

            message = QString("mythcommflag: Received a "
                              "COMMFLAG_REQUEST event for chanid %1 @ %2.  ")
                .arg(chanid).arg(recstartts.toString());

            if ((global_program_info->GetChanID()             == chanid) &&
                (global_program_info->GetRecordingStartTime() == recstartts))
            {
                commDetector->requestCommBreakMapUpdate();
                message += "Requested CommDetector to generate new break list.";
                LOG(VB_COMMFLAG, LOG_INFO, message);
            }
        }
    }
}

static int DoFlagCommercials(
    ProgramInfo *program_info,
    bool showPercentage, bool fullSpeed, int jobid,
    MythCommFlagPlayer* cfp, enum SkipTypes commDetectMethod,
    const QString &outputfilename, bool useDB)
{
    CommDetectorFactory factory;
    commDetector = factory.makeCommDetector(
        commDetectMethod, showPercentage,
        fullSpeed, cfp,
        program_info->GetChanID(),
        program_info->GetScheduledStartTime(),
        program_info->GetScheduledEndTime(),
        program_info->GetRecordingStartTime(),
        program_info->GetRecordingEndTime(), useDB);

    if (jobid > 0)
        LOG(VB_COMMFLAG, LOG_INFO,
            QString("mythcommflag processing JobID %1").arg(jobid));

    if (useDB)
        program_info->SaveCommFlagged(COMM_FLAG_PROCESSING);

    CustomEventRelayer *cer = new CustomEventRelayer(incomingCustomEvent);
    SlotRelayer *a = new SlotRelayer(commDetectorBreathe);
    SlotRelayer *b = new SlotRelayer(commDetectorStatusUpdate);
    SlotRelayer *c = new SlotRelayer(commDetectorGotNewCommercialBreakList);
    QObject::connect(commDetector, SIGNAL(breathe()),
                     a,            SLOT(relay()));
    QObject::connect(commDetector, SIGNAL(statusUpdate(const QString&)),
                     b,            SLOT(relay(const QString&)));
    QObject::connect(commDetector, SIGNAL(gotNewCommercialBreakList()),
                     c,            SLOT(relay()));

    if (useDB)
    {
        LOG(VB_COMMFLAG, LOG_INFO,
            "mythcommflag sending COMMFLAG_START notification");
        QString message = "COMMFLAG_START ";
        message += program_info->MakeUniqueKey();
        RemoteSendMessage(message);
    }

    bool result = commDetector->go();
    int comms_found = 0;

    if (result)
    {
        cfp->SaveTotalDuration();

        frm_dir_map_t commBreakList;
        commDetector->GetCommercialBreakList(commBreakList);
        comms_found = commBreakList.size() / 2;

        if (!dontSubmitCommbreakListToDB)
        {
            program_info->SaveMarkupFlag(MARK_UPDATED_CUT);
            program_info->SaveCommBreakList(commBreakList);
            program_info->SaveCommFlagged(COMM_FLAG_DONE);
        }

        print_comm_flag_output(
            program_info, commBreakList, cfp->GetTotalFrameCount(),
            (outputMethod == kOutputMethodFull) ? commDetector : NULL,
            outputfilename);
    }
    else
    {
        if (!dontSubmitCommbreakListToDB && useDB)
            program_info->SaveCommFlagged(COMM_FLAG_NOT_FLAGGED);
    }

    CommDetectorBase *tmp = commDetector;
    commDetector = NULL;
    sleep(1);
    tmp->deleteLater();

    cer->deleteLater();
    c->deleteLater();
    b->deleteLater();
    a->deleteLater();

    return comms_found;
}

static bool DoesFileExist(ProgramInfo *program_info)
{
    QString filename = get_filename(program_info);
    long long size = 0;
    bool exists = false;

    if (filename.startsWith("myth://"))
    {
        RemoteFile remotefile(filename, false, false, 0);
        struct stat filestat;

        if (remotefile.Exists(filename, &filestat))
        {
            exists = true;
            size = filestat.st_size;
        }
    }
    else
    {
        QFile file(filename);
        if (file.exists())
        {
            exists = true;
            size = file.size();
        }
    }

    if (!exists)
    {
        VERBOSE(VB_IMPORTANT, QString("Couldn't find file %1, aborting.")
                .arg(filename));
        return false;
    }

    if (size == 0)
    {
        VERBOSE(VB_IMPORTANT, QString("File %1 is zero-byte, aborting.")
                .arg(filename));
        return false;
    }

    return true;
}

static bool IsMarked(uint chanid, QDateTime starttime)
{
    MSqlQuery mark_query(MSqlQuery::InitCon());
    mark_query.prepare("SELECT commflagged, count(rm.type) "
                       "FROM recorded r "
                       "LEFT JOIN recordedmarkup rm ON "
                           "( r.chanid = rm.chanid AND "
                             "r.starttime = rm.starttime AND "
                             "type in (:MARK_START,:MARK_END)) "
                       "WHERE r.chanid = :CHANID AND "
                             "r.starttime = :STARTTIME "
                       "GROUP BY COMMFLAGGED;");
    mark_query.bindValue(":MARK_START", MARK_COMM_START);
    mark_query.bindValue(":MARK_END", MARK_COMM_END);
    mark_query.bindValue(":CHANID", chanid);
    mark_query.bindValue(":STARTTIME", starttime);

    if (mark_query.exec() && mark_query.isActive() &&
        mark_query.size() > 0)
    {
        if (mark_query.next())
        {
            int flagStatus = mark_query.value(0).toInt();
            int marksFound = mark_query.value(1).toInt();

            QString flagStatusStr = "UNKNOWN";
            switch (flagStatus) {
              case COMM_FLAG_NOT_FLAGGED:
                flagStatusStr = "Not Flagged";
                break;
              case COMM_FLAG_DONE:
                flagStatusStr = QString("Flagged with %1 breaks")
                                    .arg(marksFound / 2);
                break;
              case COMM_FLAG_PROCESSING:
                flagStatusStr = "Flagging";
                break;
              case COMM_FLAG_COMMFREE:
                flagStatusStr = "Commercial Free";
                break;
            }

            VERBOSE(VB_COMMFLAG, QString("Status for chanid %1 @ %2 is '%3'")
                               .arg(chanid).arg(starttime.toString(Qt::ISODate))
                               .arg(flagStatusStr));

            if ((flagStatus == COMM_FLAG_NOT_FLAGGED) && (marksFound == 0))
                return false;
        }
    }
    return true;
}

static int FlagCommercials(ProgramInfo *program_info, int jobid,
            const QString &outputfilename, bool useDB)
{
    global_program_info = program_info;

    int breaksFound = 0;

    // configure commercial detection method
    SkipTypes commDetectMethod = 
            (enum SkipTypes)gCoreContext->GetNumSetting(
                                    "CommercialSkipMethod", COMM_DETECT_ALL);

    if (cmdline.toBool("commmethod"))
    {
        // pull commercial detection method from command line
        QString commmethod = cmdline.toString("commmethod");

        // assume definition as integer value
        bool ok = true;
        if (cmdline.toBool("commmethod"))
            commDetectMethod = (SkipTypes) commmethod.toInt(&ok);
        if (!ok)
        {
            // not an integer, attempt comma separated list
            commDetectMethod = COMM_DETECT_UNINIT;
            QMap<QString, SkipTypes>::const_iterator sit;

            QStringList list = commmethod.split(",", QString::SkipEmptyParts);
            QStringList::const_iterator it = list.begin();
            for (; it != list.end(); ++it)
            {
                QString val = (*it).toLower();
                if (val == "off")
                {
                    commDetectMethod = COMM_DETECT_OFF;
                    break;
                }

                if (!skipTypes->contains(val))
                {
                    cerr << "Failed to decode --method option '"
                         << val.toAscii().constData()
                         << "'" << endl;
                    return GENERIC_EXIT_INVALID_CMDLINE;
                }

                // append flag method to list
                commDetectMethod = (SkipTypes) ((int)commDetectMethod
                                             || (int)skipTypes->value(val));
            }

        }
        if (commDetectMethod == COMM_DETECT_UNINIT)
            return GENERIC_EXIT_INVALID_CMDLINE;
    }
    else if (!cmdline.toBool("skipdb"))
    {
        // if not manually specified, and we have a database to access
        // pull the commflag type from the channel
        MSqlQuery query(MSqlQuery::InitCon());
        query.prepare("SELECT commmethod FROM channel "
                        "WHERE chanid = :CHANID;");
        query.bindValue(":CHANID", program_info->GetChanID());

        if (!query.exec())
        {
            // if the query fails, return with an error
            commDetectMethod = COMM_DETECT_UNINIT;
            MythDB::DBError("FlagCommercials", query);
        }
        else if (query.next())
        {
            commDetectMethod = (enum SkipTypes)query.value(0).toInt();
            if (commDetectMethod == COMM_DETECT_COMMFREE)
            {
                // if the channel is commercial free, drop to the default instead
                commDetectMethod = 
                        (enum SkipTypes)gCoreContext->GetNumSetting(
                                    "CommercialSkipMethod", COMM_DETECT_ALL);
                LOG(VB_COMMFLAG, LOG_INFO,
                        QString("Chanid %1 is marked as being Commercial Free, "
                                "we will use the default commercial detection "
                                "method").arg(program_info->GetChanID()));
            }
            else if (commDetectMethod == COMM_DETECT_UNINIT)
                // no value set, so use the database default
                commDetectMethod = 
                        (enum SkipTypes)gCoreContext->GetNumSetting(
                                     "CommercialSkipMethod", COMM_DETECT_ALL);
            LOG(VB_COMMFLAG, LOG_INFO,
                QString("Using method: %1 from channel %2")
                    .arg(commDetectMethod).arg(program_info->GetChanID()));
        }

    }

    // if selection has failed, or intentionally disabled, drop out
    if (commDetectMethod == COMM_DETECT_UNINIT)
        return GENERIC_EXIT_NOT_OK;
    else if (commDetectMethod == COMM_DETECT_OFF)
        return GENERIC_EXIT_OK;

    frm_dir_map_t blanks;
    recorder = NULL;

/*
 * is there a purpose to this not fulfilled by --getskiplist?
    if (onlyDumpDBCommercialBreakList)
    {
        frm_dir_map_t commBreakList;
        program_info->QueryCommBreakList(commBreakList);

        print_comm_flag_output(program_info, commBreakList,
                               0, NULL, outputfilename);

        global_program_info = NULL;
        return GENERIC_EXIT_OK;
    }
*/

    if (!DoesFileExist(program_info))
        return GENERIC_EXIT_PERMISSIONS_ERROR;

    QString filename = get_filename(program_info);

    RingBuffer *tmprbuf = RingBuffer::Create(filename, false);
    if (!tmprbuf)
    {
        LOG(VB_GENERAL, LOG_ERR,
            QString("Unable to create RingBuffer for %1").arg(filename));
        global_program_info = NULL;
        return GENERIC_EXIT_PERMISSIONS_ERROR;
    }

    if (useDB)
    {
        if (!MSqlQuery::testDBConnection())
        {
            VERBOSE(VB_IMPORTANT, "Unable to open commflag DB connection");
            delete tmprbuf;
            global_program_info = NULL;
            return GENERIC_EXIT_DB_ERROR;
        }
    }

    MythCommFlagPlayer *cfp = new MythCommFlagPlayer();

    PlayerContext *ctx = new PlayerContext(kFlaggerInUseID);

    AVSpecialDecode sp = (AVSpecialDecode)
        (kAVSpecialDecode_LowRes         |
         kAVSpecialDecode_SingleThreaded |
         kAVSpecialDecode_NoLoopFilter);

    /* blank detector needs to be only sample center for this optimization. */
    if ((COMM_DETECT_BLANKS  == commDetectMethod) ||
        (COMM_DETECT_2_BLANK == commDetectMethod))
    {
        sp = (AVSpecialDecode) (sp | kAVSpecialDecode_FewBlocks);
    }

    ctx->SetSpecialDecode(sp);

    ctx->SetPlayingInfo(program_info);
    ctx->SetRingBuffer(tmprbuf);
    ctx->SetPlayer(cfp);
    cfp->SetPlayerInfo(NULL, NULL, true, ctx);

    if (useDB)
    {
        if (program_info->GetRecordingEndTime() > QDateTime::currentDateTime())
        {
            gCoreContext->ConnectToMasterServer();

            recorder = RemoteGetExistingRecorder(program_info);
            if (recorder && (recorder->GetRecorderNumber() != -1))
            {
                recorderNum =  recorder->GetRecorderNumber();
                watchingRecording = true;
                ctx->SetRecorder(recorder);

                VERBOSE(VB_COMMFLAG, QString("mythcommflag will flag recording "
                        "currently in progress on cardid %1").arg(recorderNum));
            }
            else
            {
                recorderNum = -1;
                watchingRecording = false;

                LOG(VB_IMPORTANT, LOG_ERR, "Unable to find active recorder for this "
                        "recording, realtime flagging will not be enabled.");
            }
            cfp->SetWatchingRecording(watchingRecording);
        }
    }

    // TODO: Add back insertion of job if not in jobqueue

    breaksFound = DoFlagCommercials(
        program_info, progress, fullSpeed, jobid,
        cfp, commDetectMethod, outputfilename, useDB);

    if (progress)
        cerr << breaksFound << "\n";

    LOG(VB_GENERAL, LOG_NOTICE, QString("Finished, %1 break(s) found.")
        .arg(breaksFound));

    delete ctx;
    global_program_info = NULL;

    return breaksFound;
}

static int FlagCommercials( uint chanid, const QDateTime &starttime,
                            int jobid, const QString &outputfilename)
{
    QString startstring = starttime.toString("yyyyMMddhhmmss");
    ProgramInfo pginfo(chanid, starttime);

    if (!pginfo.GetChanID())
    {
        LOG(VB_GENERAL, LOG_ERR,
            QString("No program data exists for channel %1 at %2")
                .arg(chanid).arg(startstring));
        return GENERIC_EXIT_NO_RECORDING_DATA;
    }

    if (!force && JobQueue::IsJobRunning(JOB_COMMFLAG, pginfo))
    {
        if (progress)
        {
            cerr << "IN USE\n";
            cerr << "                        "
                    "(the program is already being flagged elsewhere)\n";
        }           
        VERBOSE(VB_IMPORTANT, "Program is already being flagged elsewhere");
        return GENERIC_EXIT_IN_USE;
    }
     

    if (progress)
    {   
        cerr << "MythTV Commercial Flagger, flagging commercials for:" << endl;
        if (pginfo.GetSubtitle().isEmpty())
            cerr << "    " << pginfo.GetTitle().toLocal8Bit().constData() << endl;
        else
            cerr << "    " << pginfo.GetTitle().toLocal8Bit().constData() << " - "
                 << pginfo.GetSubtitle().toLocal8Bit().constData() << endl;
    }

    return FlagCommercials(&pginfo, jobid, outputfilename, true);
}

static int FlagCommercials(QString filename, int jobid,
                const QString outputfilename, bool useDB)
{

    if (progress)
    {   
        cerr << "MythTV Commercial Flagger, flagging commercials for:" << endl
             << "    " << filename.toAscii().constData() << endl;
    }    

    ProgramInfo pginfo(filename);
    return FlagCommercials(&pginfo, jobid, outputfilename, useDB);
}

static int RebuildSeekTable(ProgramInfo *pginfo, int jobid)
{
    if (!DoesFileExist(pginfo))
        return GENERIC_EXIT_PERMISSIONS_ERROR;

    QString filename = get_filename(pginfo);

    RingBuffer *tmprbuf = RingBuffer::Create(filename, false);
    if (!tmprbuf)
    {
        LOG(VB_GENERAL, LOG_ERR,
            QString("Unable to create RingBuffer for %1").arg(filename));
        return GENERIC_EXIT_PERMISSIONS_ERROR;
    }

    MythCommFlagPlayer *cfp = new MythCommFlagPlayer();
    PlayerContext *ctx = new PlayerContext(kFlaggerInUseID);
    AVSpecialDecode sp = (AVSpecialDecode) (kAVSpecialDecode_LowRes         |
                                            kAVSpecialDecode_SingleThreaded |
                                            kAVSpecialDecode_NoLoopFilter);

    ctx->SetSpecialDecode(sp);
    ctx->SetPlayingInfo(pginfo);
    ctx->SetRingBuffer(tmprbuf);
    ctx->SetPlayer(cfp);
    cfp->SetPlayerInfo(NULL, NULL, true, ctx);

    time_t time_now;
    if (progress)
    {
        time_now = time(NULL);
        cerr << "Rebuild started at " << ctime(&time_now) << endl;
    }

    cfp->RebuildSeekTable(progress);

    if (progress)
    {
        time_now = time(NULL);
        cerr << "Rebuild completed at " << ctime(&time_now) << endl;
    }

    delete ctx;

    return GENERIC_EXIT_OK;
}

static int RebuildSeekTable(QString filename, int jobid)
{
    if (progress)
    {
        cerr << "MythTV Commercial Flagger, building seek table for:" << endl
             << "    " << filename.toAscii().constData() << endl;
    }
    ProgramInfo pginfo(filename);
    return RebuildSeekTable(&pginfo, jobid);
}

static int RebuildSeekTable(uint chanid, QDateTime starttime, int jobid)
{
    ProgramInfo pginfo(chanid, starttime);
    if (progress)
    {
        cerr << "MythTV Commercial Flagger, building seek table for:" << endl;
        if (pginfo.GetSubtitle().isEmpty())
            cerr << "    " << pginfo.GetTitle().toLocal8Bit().constData() << endl;
        else
            cerr << "    " << pginfo.GetTitle().toLocal8Bit().constData() << " - "
                 << pginfo.GetSubtitle().toLocal8Bit().constData() << endl;
    }
    return RebuildSeekTable(&pginfo, jobid);
}

int main(int argc, char *argv[])
{
    bool isVideo = false;
    int result = GENERIC_EXIT_OK;

    QString filename;
    QString outputfilename = QString::null;

    QDateTime starttime;
    QString allStart = "19700101000000";
    QString allEnd   = QDateTime::currentDateTime().toString("yyyyMMddhhmmss");
    int jobType = JOB_NONE;
    QDir fullfile;
    QString newCutList = QString::null;

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

    QCoreApplication a(argc, argv);
    QCoreApplication::setApplicationName(MYTH_APPNAME_MYTHCOMMFLAG);
    int retval;
    if ((retval = cmdline.ConfigureLogging(
                        "important general",
                        !cmdline.toBool("noprogress"))) != GENERIC_EXIT_OK)
        return retval;

    CleanupGuard callCleanup(cleanup);
    gContext = new MythContext(MYTH_BINARY_VERSION);
    if (!gContext->Init( false, /*use gui*/
                         false, /*prompt for backend*/
                         false, /*bypass auto discovery*/
                         cmdline.toBool("skipdb"))) /*ignoreDB*/
    {   
        LOG(VB_GENERAL, LOG_EMERG, "Failed to init MythContext, exiting.");
        return GENERIC_EXIT_NO_MYTHCONTEXT;
    }
    cmdline.ApplySettingsOverride();
    
    MythTranslation::load("mythfrontend");

    if (cmdline.toBool("chanid") && cmdline.toBool("starttime"))
    {
        // operate on a recording in the database
        uint chanid = cmdline.toUInt("chanid");
        QDateTime starttime = cmdline.toDateTime("starttime");

        if (cmdline.toBool("clearskiplist"))
            return ClearSkipList(chanid, starttime);
        if (cmdline.toBool("gencutlist"))
            return CopySkipListToCutList(chanid, starttime);
        if (cmdline.toBool("clearcutlist"))
            return SetCutList(chanid, starttime, "");
        if (cmdline.toBool("setcutlist"))
            return SetCutList(chanid, starttime, cmdline.toString("setcutlist"));
        if (cmdline.toBool("getcutlist"))
            return GetMarkupList("cutlist", chanid, starttime);
        if (cmdline.toBool("getskiplist"))
            return GetMarkupList("commflag", chanid, starttime);

        // TODO: check for matching jobid
        // create temporary id to operate off of if not

        if (cmdline.toBool("queue"))
            QueueCommFlagJob(chanid, starttime, cmdline.toBool("rebuild"));
        else if (cmdline.toBool("rebuild"))
            result = RebuildSeekTable(chanid, starttime, -1);
        else
            result = FlagCommercials(chanid, starttime, -1, "");
    }
    else if (cmdline.toBool("jobid"))
    {
        jobID = cmdline.toInt("jobid");
        uint chanid;
        QDateTime starttime;

        if (!JobQueue::GetJobInfoFromID(jobID, jobType, chanid, starttime))
        {
            cerr << "mythcommflag: ERROR: Unable to find DB info for "
                 << "JobQueue ID# " << jobID << endl;
            return GENERIC_EXIT_NO_RECORDING_DATA;
        }
        inJobQueue = true;
        force = true;
        int jobQueueCPU = gCoreContext->GetNumSetting("JobQueueCPU", 0);

        if (jobQueueCPU < 2)
        {
            myth_nice(17);
            myth_ioprio((0 == jobQueueCPU) ? 8 : 7);
        }

        fullSpeed = jobQueueCPU != 0;

        progress = false;
        isVideo = false;

        int ret = 0;

        if (JobQueue::GetJobFlags(jobID) && JOB_REBUILD)
            RebuildSeekTable(chanid, starttime, jobID);
        else
            ret = FlagCommercials(chanid, starttime, jobID, "");

        if (ret > GENERIC_EXIT_NOT_OK)
            JobQueue::ChangeJobStatus(jobID, JOB_ERRORED,
                        QString("Failed with exit status %1").arg(ret));
        else
            JobQueue::ChangeJobStatus(jobID, JOB_FINISHED,
                        QString("%1 commercial breaks").arg(ret));
    }
    else if (cmdline.toBool("video"))
    {
        // build skiplist for video file
        return RebuildSeekTable(cmdline.toString(filename), -1);
    }
    else if (cmdline.toBool("file"))
    {
        // TODO: add back handling of recording defined by the basename

        // perform commercial flagging on file outside the database
        FlagCommercials(cmdline.toString("file"), -1,
                        cmdline.toString("outputfile"),
                        !cmdline.toBool("skipdb"));
    }
    else if (cmdline.toBool("queue"))
    {
        // run flagging for all recordings with no skiplist
        MSqlQuery query(MSqlQuery::InitCon());
        query.prepare("SELECT r.chanid, r.starttime, c.commmethod "
                        "FROM recorded AS r "
                   "LEFT JOIN channel AS c ON r.chanid=c.chanid "
//                     "WHERE startime >= :STARTTIME AND endtime <= :ENDTIME "
                    "ORDER BY starttime;");
        //query.bindValue(":STARTTIME", allStart);
        //query.bindValue(":ENDTIME", allEnd);
        
        if (query.exec() && query.isActive() && query.size() > 0)
        {
            QDateTime starttime;
            uint chanid;

            while (query.next())
            {
                starttime = QDateTime::fromString(query.value(1).toString(),
                                                        Qt::ISODate);
                chanid = query.value(0).toUInt();

                if (!cmdline.toBool("force") && !cmdline.toBool("rebuild"))
                {
                    // recording is already flagged
                    if (IsMarked(chanid, starttime))
                        continue;

                    // channel is marked as commercial free
                    if (query.value(2).toInt() == COMM_DETECT_COMMFREE)
                        continue;

                    // recording rule did not enable commflagging
//                    RecordingInfo recinfo(chanid, starttime);
//                    if (!(recinfo.GetAutoRunJobs() & JOB_COMMFLAG))
//                        continue;
                }

                QueueCommFlagJob(chanid, starttime, cmdline.toBool("rebuild"));
            }
        }

    }
    else
    {
        VERBOSE(VB_IMPORTANT, "No valid combination of command inputs received.");
        cmdline.PrintHelp();
        return GENERIC_EXIT_INVALID_CMDLINE;
    }

    return result;
}


/* vim: set expandtab tabstop=4 shiftwidth=4: */
