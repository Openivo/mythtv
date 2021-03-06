// qt
#include <QCoreApplication>
#include <QEvent>
#include <QDir>
#include <QUrl>

// myth
#include "mythcorecontext.h"
#include "mythdirs.h"
#include "mythuihelper.h"
#include "mythsystem.h"
#include "metadatadownload.h"
#include "util.h"
#include "remotefile.h"
#include "mythlogging.h"

QEvent::Type MetadataLookupEvent::kEventType =
    (QEvent::Type) QEvent::registerEventType();

QEvent::Type MetadataLookupFailure::kEventType =
    (QEvent::Type) QEvent::registerEventType();

MetadataDownload::MetadataDownload(QObject *parent)
{
    m_parent = parent;
}

MetadataDownload::~MetadataDownload()
{
    cancel();
    wait();
}

void MetadataDownload::addLookup(MetadataLookup *lookup)
{
    // Add a lookup to the queue
    m_mutex.lock();
    m_lookupList.append(lookup);
    if (!isRunning())
        start();
    m_mutex.unlock();
}

void MetadataDownload::prependLookup(MetadataLookup *lookup)
{
    // Add a lookup to the queue
    m_mutex.lock();
    m_lookupList.prepend(lookup);
    if (!isRunning())
        start();
    m_mutex.unlock();
}

void MetadataDownload::cancel()
{
    m_mutex.lock();
    qDeleteAll(m_lookupList);
    m_lookupList.clear();
    m_parent = NULL;
    m_mutex.unlock();
}

void MetadataDownload::run()
{
    MetadataLookup* lookup;
    threadRegister("MetadataDownload");
    while ((lookup = moreWork()) != NULL)
    {
        MetadataLookupList list;
        // Go go gadget Metadata Lookup
        if (lookup->GetType() == VID || lookup->GetType() == RECDNG)
        {
            if (lookup->GetSeason() > 0 || lookup->GetEpisode() > 0)
                list = handleTelevision(lookup);
            else if (!lookup->GetSubtitle().isEmpty() &&
                     lookup->GetType() == VID)
                list = handleVideoUndetermined(lookup);
            else if (lookup->GetType() == RECDNG)
                list = handleRecordingUndetermined(lookup);
            else
                list = handleMovie(lookup);
        }
//        else if (lookup->GetType() == MUSIC)
//            list = handleMusic(lookup);
        else if (lookup->GetType() == GAME)
            list = handleGame(lookup);

        // inform parent we have lookup ready for it
        if (m_parent && list.count() >= 1)
        {
            // If there's only one result, don't bother asking
            // our parent about it, just add it to the back of
            // the queue in GETDATA mode.
            if (list.count() == 1 && list.at(0)->GetStep() == SEARCH)
            {
                MetadataLookup *newlookup = list.takeFirst();
                newlookup->SetStep(GETDATA);
                prependLookup(newlookup);
                continue;
            }

            // If we're in automatic mode, we need to make
            // these decisions on our own.  Pass to title match.
            if (list.at(0)->GetAutomatic() && list.count() > 1)
            {
                if (!findBestMatch(list, lookup->GetTitle()))
                    QCoreApplication::postEvent(m_parent,
                        new MetadataLookupFailure(MetadataLookupList() << lookup));
                continue;
            }

            LOG(VB_GENERAL, LOG_INFO,
                QString("Returning Metadata Results: %1 %2 %3")
                    .arg(lookup->GetTitle()).arg(lookup->GetSeason())
                    .arg(lookup->GetEpisode()));
            QCoreApplication::postEvent(m_parent,
                new MetadataLookupEvent(list));
        }
        else
        {
            list.append(lookup);
            QCoreApplication::postEvent(m_parent,
                new MetadataLookupFailure(list));
        }
    }
    threadDeregister();
}

MetadataLookup* MetadataDownload::moreWork()
{
    MetadataLookup* result = NULL;
    m_mutex.lock();
    if (!m_lookupList.isEmpty())
        result = m_lookupList.takeFirst();
    m_mutex.unlock();
    return result;
}

bool MetadataDownload::findBestMatch(MetadataLookupList list,
                                           QString originaltitle)
{
    QStringList titles;

    // Build a list of all the titles
    for (MetadataLookupList::const_iterator i = list.begin();
            i != list.end(); ++i)
    {
        titles.append((*i)->GetTitle());
    }

    // Apply Levenshtein distance algorithm to determine closest match
    QString bestTitle = nearestName(originaltitle, titles);

    // If no "best" was chosen, give up.
    if (bestTitle.isEmpty())
    {
        LOG(VB_GENERAL, LOG_ERR,
            QString("No adequate match or multiple "
                    "matches found for %1.  Update manually.")
                    .arg(originaltitle));
        return false;
    }

    LOG(VB_GENERAL, LOG_INFO, QString("Best Title Match For %1: %2")
                    .arg(originaltitle).arg(bestTitle));

    // Grab the one item that matches the besttitle (IMPERFECT)
    for (MetadataLookupList::const_iterator i = list.begin();
            i != list.end(); ++i)
    {
        if ((*i)->GetTitle() == bestTitle)
        {
            MetadataLookup *newlookup = (*i);
            newlookup->SetStep(GETDATA);

            // If searching for TV without a subtitle, but we've found
            // a series match, arbitrarily set season/episode to "1" to
            // avoid looping forever trying to figure it out.

            if (newlookup->GetType() == RECDNG)
            {
                newlookup->SetSeason(1);
                newlookup->SetEpisode(1);
            }

            prependLookup(newlookup);
            return true;
        }
    }

    return false;
}

MetadataLookupList MetadataDownload::runGrabber(QString cmd, QStringList args,
                                                MetadataLookup* lookup,
                                                bool passseas)
{
    MythSystem grabber(cmd, args, kMSRunShell | kMSStdOut | kMSBuffered);
    MetadataLookupList list;

    LOG(VB_GENERAL, LOG_INFO, QString("Running Grabber: %1 %2")
        .arg(cmd).arg(args.join(" ")));

    grabber.Run();
    grabber.Wait();
    QByteArray result = grabber.ReadAll();
    if (!result.isEmpty())
    {
        QDomDocument doc;
        doc.setContent(result, true);
        QDomElement root = doc.documentElement();
        QDomElement item = root.firstChildElement("item");

        while (!item.isNull())
        {
            MetadataLookup *tmp = ParseMetadataItem(item, lookup,
                passseas);
            list.append(tmp);
            item = item.nextSiblingElement("item");
        }
    }
    return list;
}

MetadataLookupList MetadataDownload::readMXML(QString MXMLpath,
                                             MetadataLookup* lookup,
                                             bool passseas)
{
    MetadataLookupList list;

    LOG(VB_GENERAL, LOG_INFO,
        QString("Matching MXML file found. Parsing %1 for metadata...")
               .arg(MXMLpath));

    if (lookup->GetType() == VID)
    {
        QByteArray mxmlraw;
        QDomElement item;
        if (MXMLpath.startsWith("myth://"))
        {
            RemoteFile *rf = new RemoteFile(MXMLpath);
            if (rf && rf->Open())
            {
                bool loaded = rf->SaveAs(mxmlraw);
                if (loaded)
                {
                    QDomDocument doc;
                    if (doc.setContent(mxmlraw, true))
                    {
                        lookup->SetStep(GETDATA);
                        QDomElement root = doc.documentElement();
                        item = root.firstChildElement("item");
                    }
                    else
                        LOG(VB_GENERAL, LOG_ERR,
                            QString("Corrupt or invalid MXML file."));
                }
                rf->Close();
            }

            delete rf;
            rf = NULL;
        }
        else
        {
            QFile file(MXMLpath);
            if (file.open(QIODevice::ReadOnly))
            {
                mxmlraw = file.readAll();
                QDomDocument doc;
                if (doc.setContent(mxmlraw, true))
                {
                    lookup->SetStep(GETDATA);
                    QDomElement root = doc.documentElement();
                    item = root.firstChildElement("item");
                }
                else
                    LOG(VB_GENERAL, LOG_ERR,
                        QString("Corrupt or invalid MXML file."));
                file.close();
            }
        }

        MetadataLookup *tmp = ParseMetadataItem(item, lookup, passseas);
        list.append(tmp);
    }

    return list;
}

MetadataLookupList MetadataDownload::readNFO(QString NFOpath,
                                             MetadataLookup* lookup)
{
    MetadataLookupList list;

    LOG(VB_GENERAL, LOG_INFO,
        QString("Matching NFO file found. Parsing %1 for metadata...")
               .arg(NFOpath));

    if (lookup->GetType() == VID)
    {
        QByteArray nforaw;
        QDomElement item;
        if (NFOpath.startsWith("myth://"))
        {
            RemoteFile *rf = new RemoteFile(NFOpath);
            if (rf && rf->Open())
            {
                bool loaded = rf->SaveAs(nforaw);
                if (loaded)
                {
                    QDomDocument doc;
                    if (doc.setContent(nforaw, true))
                    {
                        lookup->SetStep(GETDATA);
                        item = doc.documentElement();
                    }
                    else
                        LOG(VB_GENERAL, LOG_ERR,
                            QString("PIRATE ERROR: Invalid NFO file found."));
                }
                rf->Close();
            }

            delete rf;
            rf = NULL;
        }
        else
        {
            QFile file(NFOpath);
            if (file.open(QIODevice::ReadOnly))
            {
                nforaw = file.readAll();
                QDomDocument doc;
                if (doc.setContent(nforaw, true))
                {
                    lookup->SetStep(GETDATA);
                    item = doc.documentElement();
                }
                else
                    LOG(VB_GENERAL, LOG_ERR,
                        QString("PIRATE ERROR: Invalid NFO file found."));
                file.close();
            }
        }

        MetadataLookup *tmp = ParseMetadataMovieNFO(item, lookup);
        list.append(tmp);
    }

    return list;
}

MetadataLookupList MetadataDownload::handleGame(MetadataLookup* lookup)
{
    MetadataLookupList list;

    QString def_cmd = QDir::cleanPath(QString("%1/%2")
        .arg(GetShareDir())
        .arg("metadata/Game/giantbomb.py"));

    QString cmd = gCoreContext->GetSetting("mythgame.MetadataGrabber", def_cmd);

    QStringList args;
    args.append(QString("-l")); // Language Flag
    args.append(gCoreContext->GetLanguage()); // UI Language

    // If the inetref is populated, even in search mode,
    // become a getdata grab and use that.
    if (lookup->GetStep() == SEARCH &&
        (!lookup->GetInetref().isEmpty() &&
         lookup->GetInetref() != "00000000"))
        lookup->SetStep(GETDATA);

    if (lookup->GetStep() == SEARCH)
    {
        args.append(QString("-M"));
        QString title = lookup->GetTitle();
        args.append(ShellEscape(title));
    }
    else if (lookup->GetStep() == GETDATA)
    {
        args.append(QString("-D"));
        args.append(lookup->GetInetref());
    }
    list = runGrabber(cmd, args, lookup);

    return list;
}

MetadataLookupList MetadataDownload::handleMovie(MetadataLookup* lookup)
{
    MetadataLookupList list;

    QString mxml = getMXMLPath(lookup->GetFilename());
    QString nfo = getNFOPath(lookup->GetFilename());

    if (mxml.isEmpty() && nfo.isEmpty())
    {
        QString def_cmd = QDir::cleanPath(QString("%1/%2")
            .arg(GetShareDir())
            .arg("metadata/Movie/tmdb.py"));

        QString cmd = gCoreContext->GetSetting("MovieGrabber", def_cmd);

        QStringList args;
        args.append(QString("-l")); // Language Flag
        args.append(gCoreContext->GetLanguage()); // UI Language

        // If the inetref is populated, even in search mode,
        // become a getdata grab and use that.
        if (lookup->GetStep() == SEARCH &&
            (!lookup->GetInetref().isEmpty() &&
             lookup->GetInetref() != "00000000"))
            lookup->SetStep(GETDATA);

        if (lookup->GetStep() == SEARCH)
        {
            args.append(QString("-M"));
            QString title = lookup->GetTitle();
            args.append(ShellEscape(title));
        }
        else if (lookup->GetStep() == GETDATA)
        {
            args.append(QString("-D"));
            args.append(lookup->GetInetref());
        }
        list = runGrabber(cmd, args, lookup);
    }
    else if (!mxml.isEmpty())
        list = readMXML(mxml, lookup);
    else if (!nfo.isEmpty())
        list = readNFO(nfo, lookup);

    return list;
}

MetadataLookupList MetadataDownload::handleTelevision(MetadataLookup* lookup)
{
    MetadataLookupList list;

    QString def_cmd = QDir::cleanPath(QString("%1/%2")
        .arg(GetShareDir())
        .arg("metadata/Television/ttvdb.py"));

    QString cmd = gCoreContext->GetSetting("TelevisionGrabber", def_cmd);

    QStringList args;
    args.append(QString("-l")); // Language Flag
    args.append(gCoreContext->GetLanguage()); // UI Language

    // If the inetref is populated, even in search mode,
    // become a getdata grab and use that.
    if (lookup->GetStep() == SEARCH &&
        (!lookup->GetInetref().isEmpty() &&
         lookup->GetInetref() != "00000000"))
        lookup->SetStep(GETDATA);

    if (lookup->GetStep() == SEARCH)
    {
        args.append(QString("-M"));
        QString title = lookup->GetTitle();
        args.append(ShellEscape(title));
    }
    else if (lookup->GetStep() == GETDATA)
    {
        args.append(QString("-D"));
        args.append(lookup->GetInetref());
        args.append(QString::number(lookup->GetSeason()));
        args.append(QString::number(lookup->GetEpisode()));
    }
    list = runGrabber(cmd, args, lookup);
    return list;
}

MetadataLookupList MetadataDownload::handleVideoUndetermined(
                                                    MetadataLookup* lookup)
{
    MetadataLookupList list;

    QString def_cmd = QDir::cleanPath(QString("%1/%2")
        .arg(GetShareDir())
        .arg("metadata/Television/ttvdb.py"));

    QString cmd = gCoreContext->GetSetting("TelevisionGrabber", def_cmd);

    // Can't trust the inetref with so little information.

    QStringList args;
    args.append(QString("-l")); // Language Flag
    args.append(gCoreContext->GetLanguage()); // UI Language
    args.append(QString("-N"));
    QString title = lookup->GetTitle();
    args.append(ShellEscape(title));
    QString subtitle = lookup->GetSubtitle();
    args.append(ShellEscape(subtitle));

    // Try to do a title/subtitle lookup
    list = runGrabber(cmd, args, lookup, false);

    // If there were no results for that, fall back to a movie lookup.
    if (!list.size())
        list = handleMovie(lookup);

    if (list.count() == 1)
        list.at(0)->SetStep(GETDATA);

    return list;
}

MetadataLookupList MetadataDownload::handleRecordingUndetermined(
                                                    MetadataLookup* lookup)
{
    MetadataLookupList list;

    QString def_cmd = QDir::cleanPath(QString("%1/%2")
        .arg(GetShareDir())
        .arg("metadata/Television/ttvdb.py"));

    QString cmd = gCoreContext->GetSetting("TelevisionGrabber", def_cmd);

    QStringList args;
    args.append(QString("-l")); // Language Flag
    args.append(gCoreContext->GetLanguage()); // UI Language
    if (!lookup->GetSubtitle().isEmpty())
        args.append(QString("-N"));
    else
    {
        // The input lookup doesn't have Subtitle, Season or Episode information.
        // We're going to "artificially" set Seas/Ep to 1 since the input
        // isn't enough information to get conclusive metadata anyway.
        // This is needed in case of a multi-result, so that on the second
        // pass through, we definitely get the TV grabber and at least get
        // an inetref.
        lookup->SetSeason(1);
        lookup->SetEpisode(1);
        args.append(QString("-M"));
    }

    if (!lookup->GetInetref().isEmpty())
    {
        QString inetref = lookup->GetInetref();
        args.append(ShellEscape(inetref));
    }
    else
    {
        QString title = lookup->GetTitle();
        args.append(ShellEscape(title));
    }

    if (!lookup->GetSubtitle().isEmpty())
    {
        QString subtitle = lookup->GetSubtitle();
        args.append(ShellEscape(subtitle));
        list = runGrabber(cmd, args, lookup, false);
    }
    else
        list = runGrabber(cmd, args, lookup, true);

    // If there were no results for that, fall back to a movie lookup.
    if (!list.size())
    {
        lookup->SetSeason(0);
        lookup->SetEpisode(0);
        list = handleMovie(lookup);
    }

    if (list.count() == 1)
        list.at(0)->SetStep(GETDATA);

    return list;
}

QString MetadataDownload::getMXMLPath(QString filename)
{
    QString ret;
    QString xmlname;
    QUrl qurl(filename);
    QString ext = QFileInfo(qurl.path()).suffix();
    xmlname = filename.left(filename.size() - ext.size()) + "mxml";

    if (xmlname.startsWith("myth://"))
    {
        if (RemoteFile::Exists(xmlname))
            ret = xmlname;
    }
    else
    {
        if (QFile::exists(xmlname))
            ret = xmlname;
    }

    return ret;
}

QString MetadataDownload::getNFOPath(QString filename)
{
    QString ret;
    QString nfoname;
    QUrl qurl(filename);
    QString ext = QFileInfo(qurl.path()).suffix();
    nfoname = filename.left(filename.size() - ext.size()) + "nfo";

    if (nfoname.startsWith("myth://"))
    {
        if (RemoteFile::Exists(nfoname))
            ret = nfoname;
    }
    else
    {
        if (QFile::exists(nfoname))
            ret = nfoname;
    }

    return ret;
}
