#include "library/rekordbox/rekordboxfeature.h"

#include <mp3guessenc.h>
#include <rekordbox_anlz.h>
#include <rekordbox_pdb.h>

#include <QMap>
#include <QMessageBox>
#include <QSettings>
#include <QString>
#include <QTextCodec>
#include <QtDebug>

#include "engine/engine.h"
#include "library/dao/trackschema.h"
#include "library/library.h"
#include "library/queryutil.h"
#include "library/rekordbox/rekordboxconstants.h"
#include "library/trackcollection.h"
#include "library/trackcollectionmanager.h"
#include "library/treeitem.h"
#include "moc_rekordboxfeature.cpp"
#include "track/beats.h"
#include "track/cue.h"
#include "track/keyfactory.h"
#include "track/track.h"
#include "util/color/color.h"
#include "util/db/dbconnectionpooled.h"
#include "util/db/dbconnectionpooler.h"
#include "util/sandbox.h"
#include "waveform/waveform.h"
#include "widget/wlibrary.h"
#include "widget/wlibrarytextbrowser.h"

#define IS_RECORDBOX_DEVICE "::isRecordboxDevice::"
#define IS_NOT_RECORDBOX_DEVICE "::isNotRecordboxDevice::"

namespace {

const QString kRekordboxLibraryTable = QStringLiteral("rekordbox_library");
const QString kRekordboxPlaylistsTable = QStringLiteral("rekordbox_playlists");
const QString kRekordboxPlaylistTracksTable = QStringLiteral("rekordbox_playlist_tracks");

const QString kPdbPath = QStringLiteral("PIONEER/rekordbox/export.pdb");
const QString kPLaylistPathDelimiter = QStringLiteral("-->");

enum class IDForColor : uint8_t {
    Pink = 1,
    Red,
    Orange,
    Yellow,
    Green,
    Aqua,
    Blue,
    Purple
};

constexpr mixxx::RgbColor kColorForIDPink(0xF870F8);
constexpr mixxx::RgbColor kColorForIDRed(0xF870900);
constexpr mixxx::RgbColor kColorForIDOrange(0xF8A030);
constexpr mixxx::RgbColor kColorForIDYellow(0xF8E331);
constexpr mixxx::RgbColor kColorForIDGreen(0x1EE000);
constexpr mixxx::RgbColor kColorForIDAqua(0x16C0F8);
constexpr mixxx::RgbColor kColorForIDBlue(0x0150F8);
constexpr mixxx::RgbColor kColorForIDPurple(0x9808F8);
constexpr mixxx::RgbColor kColorForIDNoColor(0x0);

struct memory_cue_loop_t {
    mixxx::audio::FramePos startPosition;
    mixxx::audio::FramePos endPosition;
    QString comment;
    mixxx::RgbColor::optional_t color;
};

bool createLibraryTable(QSqlDatabase& database, const QString& tableName) {
    qDebug() << "Creating Rekordbox library table: " << tableName;

    QSqlQuery query(database);
    query.prepare(
            "CREATE TABLE IF NOT EXISTS " + tableName +
            " ("
            "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "    rb_id INTEGER,"
            "    artist TEXT,"
            "    title TEXT,"
            "    album TEXT,"
            "    year INTEGER,"
            "    genre TEXT,"
            "    tracknumber TEXT,"
            "    location TEXT UNIQUE,"
            "    comment TEXT,"
            "    duration INTEGER,"
            "    bitrate TEXT,"
            "    bpm FLOAT,"
            "    key TEXT,"
            "    rating INTEGER,"
            "    analyze_path TEXT UNIQUE,"
            "    device TEXT,"
            "    color INTEGER"
            ");");

    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
        return false;
    }

    return true;
}

bool createPlaylistsTable(QSqlDatabase& database, const QString& tableName) {
    qDebug() << "Creating Rekordbox playlists table: " << tableName;

    QSqlQuery query(database);
    query.prepare(
            "CREATE TABLE IF NOT EXISTS " + tableName +
            " ("
            "    id INTEGER PRIMARY KEY,"
            "    name TEXT UNIQUE"
            ");");

    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
        return false;
    }

    return true;
}

bool createPlaylistTracksTable(QSqlDatabase& database, const QString& tableName) {
    qDebug() << "Creating Rekordbox playlist tracks table: " << tableName;

    QSqlQuery query(database);
    query.prepare(
            "CREATE TABLE IF NOT EXISTS " + tableName +
            " ("
            "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "    playlist_id INTEGER REFERENCES rekordbox_playlists(id),"
            "    track_id INTEGER REFERENCES rekordbox_library(id),"
            "    position INTEGER"
            ");");

    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
        return false;
    }

    return true;
}

bool dropTable(QSqlDatabase& database, const QString& tableName) {
    qDebug() << "Dropping Rekordbox table: " << tableName;

    QSqlQuery query(database);
    query.prepare("DROP TABLE IF EXISTS " + tableName);

    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
        return false;
    }

    return true;
}

// This function is executed in a separate thread other than the main thread
// The returned list owns the pointers, but we can't use a unique_ptr because
// the result is passed by a const reference inside QFuture and than copied
// to the main thread requiring a copy-able object.
QList<TreeItem*> findRekordboxDevices() {
    QThread* thisThread = QThread::currentThread();
    thisThread->setPriority(QThread::LowPriority);

    QList<TreeItem*> foundDevices;

#if defined(__WINDOWS__)
    // Repopulate drive list
    QFileInfoList drives = QDir::drives();
    // show drive letters
    foreach (QFileInfo drive, drives) {
        // Using drive.filePath() instead of drive.canonicalPath() as it
        // freezes interface too much if there is a network share mounted
        // (drive letter assigned) but unavailable
        //
        // drive.canonicalPath() make a system call to the underlying filesystem
        // introducing delay if it is unreadable.
        // drive.filePath() doesn't make any access to the filesystem and consequently
        // shorten the delay

        QFileInfo rbDBFileInfo(drive.filePath() + kPdbPath);

        if (rbDBFileInfo.exists() && rbDBFileInfo.isFile()) {
            QString displayPath = drive.filePath();
            if (displayPath.endsWith("/")) {
                displayPath.chop(1);
            }
            QList<QString> data;
            data << drive.filePath();
            data << IS_RECORDBOX_DEVICE;
            auto* pFoundDevice = new TreeItem(
                    std::move(displayPath),
                    QVariant(data));
            foundDevices << pFoundDevice;
        }
    }
#elif defined(__LINUX__)
    // To get devices on Linux, we look for directories under /media and
    // /run/media/$USER.
    QFileInfoList devices;

    // Add folders under /media to devices.
    devices += QDir(QStringLiteral("/media")).entryInfoList(QDir::AllDirs | QDir::NoDotAndDotDot);

    // Add folders under /media/$USER to devices.
    QDir mediaUserDir(QStringLiteral("/media/") + QString::fromLocal8Bit(qgetenv("USER")));
    devices += mediaUserDir.entryInfoList(
            QDir::AllDirs | QDir::NoDotAndDotDot);

    // Add folders under /run/media/$USER to devices.
    QDir runMediaUserDir(QStringLiteral("/run/media/") + QString::fromLocal8Bit(qgetenv("USER")));
    devices += runMediaUserDir.entryInfoList(
            QDir::AllDirs | QDir::NoDotAndDotDot);

    foreach (QFileInfo device, devices) {
        QFileInfo rbDBFileInfo(device.filePath() + QStringLiteral("/") + kPdbPath);

        if (rbDBFileInfo.exists() && rbDBFileInfo.isFile()) {
            auto* pFoundDevice = new TreeItem(
                    device.fileName(),
                    QVariant(QList<QString>{device.filePath(), IS_RECORDBOX_DEVICE}));
            foundDevices << pFoundDevice;
        }
    }
#else // __APPLE__
    QFileInfoList devices = QDir(QStringLiteral("/Volumes")).entryInfoList(QDir::AllDirs | QDir::NoDotAndDotDot);

    foreach (QFileInfo device, devices) {
        QFileInfo rbDBFileInfo(device.filePath() + QStringLiteral("/") + kPdbPath);

        if (rbDBFileInfo.exists() && rbDBFileInfo.isFile()) {
            QList<QString> data;
            data << device.filePath();
            data << IS_RECORDBOX_DEVICE;
            auto* pFoundDevice = new TreeItem(
                    device.fileName(),
                    QVariant(data));
            foundDevices << pFoundDevice;
        }
    }
#endif

    return foundDevices;
}

template<typename Base, typename T>
inline bool instanceof (const T* ptr) {
    return dynamic_cast<const Base*>(ptr) != nullptr;
}

QString fromUtf16LeString(const std::string& toConvert) {
    // Kaitai uses std::string as single container for all string encodings.
    return QTextCodec::codecForName("UTF-16LE")
            ->toUnicode(toConvert.data(), static_cast<int>(toConvert.length()));
}

QString fromUtf16BeString(const std::string& toConvert) {
    // Kaitai uses std::string as single container for all string encodings.
    int length = static_cast<int>(toConvert.length()) - 2; // strip off trailing nullbyte
    return QTextCodec::codecForName("UTF-16BE")->toUnicode(toConvert.data(), length);
}

// Functions getText and parseDeviceDB are roughly based on the following Java file:
// https://github.com/Deep-Symmetry/crate-digger/commit/f09fa9fc097a2a428c43245ddd542ac1370c1adc
// getText is needed because the strings in the PDB file "have a variety of obscure representations".

QString getText(rekordbox_pdb_t::device_sql_string_t* deviceString) {
    QString text;

    if (instanceof <rekordbox_pdb_t::device_sql_short_ascii_t>(deviceString->body())) {
        rekordbox_pdb_t::device_sql_short_ascii_t* shortAsciiString =
                static_cast<rekordbox_pdb_t::device_sql_short_ascii_t*>(deviceString->body());
        text = QString::fromStdString(shortAsciiString->text());
    } else if (instanceof <rekordbox_pdb_t::device_sql_long_ascii_t>(deviceString->body())) {
        rekordbox_pdb_t::device_sql_long_ascii_t* longAsciiString =
                static_cast<rekordbox_pdb_t::device_sql_long_ascii_t*>(deviceString->body());
        text = QString::fromStdString(longAsciiString->text());
    } else if (instanceof <rekordbox_pdb_t::device_sql_long_utf16le_t>(deviceString->body())) {
        rekordbox_pdb_t::device_sql_long_utf16le_t* longUtf16leString =
                static_cast<rekordbox_pdb_t::device_sql_long_utf16le_t*>(deviceString->body());
        text = fromUtf16LeString(longUtf16leString->text());
    }

    // Some strings read from Rekordbox *.PDB files contain random null characters
    // which if not removed cause Mixxx to crash when attempting to read file paths
    return text.remove(QChar('\x0'));
}

int createDevicePlaylist(QSqlDatabase& database, const QString& devicePath) {
    int playlistID = kInvalidPlaylistId;

    QSqlQuery queryInsertIntoDevicePlaylist(database);
    queryInsertIntoDevicePlaylist.prepare(
            "INSERT INTO " + kRekordboxPlaylistsTable +
            " (name) "
            "VALUES (:name)");

    queryInsertIntoDevicePlaylist.bindValue(":name", devicePath);

    if (!queryInsertIntoDevicePlaylist.exec()) {
        LOG_FAILED_QUERY(queryInsertIntoDevicePlaylist)
                << "devicePath: " << devicePath;
        return playlistID;
    }

    QSqlQuery idQuery(database);
    idQuery.prepare("select id from " + kRekordboxPlaylistsTable + " where name=:path");
    idQuery.bindValue(":path", devicePath);

    if (!idQuery.exec()) {
        LOG_FAILED_QUERY(idQuery)
                << "devicePath: " << devicePath;
        return playlistID;
    }

    while (idQuery.next()) {
        playlistID = idQuery.value(idQuery.record().indexOf("id")).toInt();
    }

    return playlistID;
}

mixxx::RgbColor colorFromID(int colorID) {
    switch (static_cast<IDForColor>(colorID)) {
    case IDForColor::Pink:
        return kColorForIDPink;
    case IDForColor::Red:
        return kColorForIDRed;
    case IDForColor::Orange:
        return kColorForIDOrange;
    case IDForColor::Yellow:
        return kColorForIDYellow;
    case IDForColor::Green:
        return kColorForIDGreen;
    case IDForColor::Aqua:
        return kColorForIDAqua;
    case IDForColor::Blue:
        return kColorForIDBlue;
    case IDForColor::Purple:
        return kColorForIDPurple;
    }
    return kColorForIDNoColor;
}

void insertTrack(
        QSqlDatabase& database,
        rekordbox_pdb_t::track_row_t* track,
        QSqlQuery& query,
        QSqlQuery& queryInsertIntoDevicePlaylistTracks,
        QMap<uint32_t, QString>& artistsMap,
        QMap<uint32_t, QString>& albumsMap,
        QMap<uint32_t, QString>& genresMap,
        QMap<uint32_t, QString>& keysMap,
        const QString& devicePath,
        const QString& device,
        int audioFilesCount) {
    int rbID = static_cast<int>(track->id());
    QString title = getText(track->title());
    QString artist = artistsMap[track->artist_id()];
    QString album = albumsMap[track->album_id()];
    QString year = QString::number(track->year());
    QString genre = genresMap[track->genre_id()];
    QString location = devicePath + getText(track->file_path());
    float bpm = static_cast<float>(track->tempo() / 100.0);
    int bitrate = static_cast<int>(track->bitrate());
    QString key = keysMap[track->key_id()];
    int playtime = static_cast<int>(track->duration());
    int rating = static_cast<int>(track->rating());
    QString comment = getText(track->comment());
    QString tracknumber = QString::number(track->track_number());
    QString anlzPath = devicePath + getText(track->analyze_path());

    query.bindValue(":rb_id", rbID);
    query.bindValue(":artist", artist);
    query.bindValue(":title", title);
    query.bindValue(":album", album);
    query.bindValue(":genre", genre);
    query.bindValue(":year", year);
    query.bindValue(":duration", playtime);
    query.bindValue(":location", location);
    query.bindValue(":rating", rating);
    query.bindValue(":comment", comment);
    query.bindValue(":tracknumber", tracknumber);
    query.bindValue(":key", key);
    query.bindValue(":bpm", bpm);
    query.bindValue(":bitrate", bitrate);
    query.bindValue(":analyze_path", anlzPath);
    query.bindValue(":device", device);
    query.bindValue(":color",
            mixxx::RgbColor::toQVariant(
                    colorFromID(static_cast<int>(track->color_id()))));

    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
    }

    int trackID = -1;
    QSqlQuery finderQuery(database);
    finderQuery.prepare("select id from " + kRekordboxLibraryTable +
            " where rb_id=:rb_id and device=:device");
    finderQuery.bindValue(":rb_id", rbID);
    finderQuery.bindValue(":device", device);

    if (!finderQuery.exec()) {
        LOG_FAILED_QUERY(finderQuery)
                << "rbID:" << rbID;
    }

    if (finderQuery.next()) {
        trackID = finderQuery.value(finderQuery.record().indexOf("id")).toInt();
    }

    // Insert into device all tracks playlist
    queryInsertIntoDevicePlaylistTracks.bindValue(":track_id", trackID);
    queryInsertIntoDevicePlaylistTracks.bindValue(":position", audioFilesCount);

    if (!queryInsertIntoDevicePlaylistTracks.exec()) {
        LOG_FAILED_QUERY(queryInsertIntoDevicePlaylistTracks)
                << "trackID:" << trackID
                << "position:" << audioFilesCount;
    }
}

void buildPlaylistTree(
        QSqlDatabase& database,
        TreeItem* parent,
        uint32_t parentID,
        QMap<uint32_t, QString>& playlistNameMap,
        QMap<uint32_t, bool>& playlistIsFolderMap,
        QMap<uint32_t, QMap<uint32_t, uint32_t>>& playlistTreeMap,
        QMap<uint32_t, QMap<uint32_t, uint32_t>>& playlistTrackMap,
        const QString& playlistPath,
        const QString& device);

QString parseDeviceDB(mixxx::DbConnectionPoolPtr dbConnectionPool, TreeItem* deviceItem) {
    QString device = deviceItem->getLabel();
    QString devicePath = deviceItem->getData().toList()[0].toString();

    qDebug() << "parseDeviceDB device: " << device << " devicePath: " << devicePath;

    QString dbPath = devicePath + QStringLiteral("/") + kPdbPath;

    if (!QFile(dbPath).exists()) {
        return devicePath;
    }

    // The pooler limits the lifetime all thread-local connections,
    // that should be closed immediately before exiting this function.
    const mixxx::DbConnectionPooler dbConnectionPooler(dbConnectionPool);
    QSqlDatabase database = mixxx::DbConnectionPooled(dbConnectionPool);

    //Open the database connection in this thread.
    VERIFY_OR_DEBUG_ASSERT(database.isOpen()) {
        qDebug() << "Failed to open database for Rekordbox parser."
                 << database.lastError();
        return QString();
    }

    //Give thread a low priority
    QThread* thisThread = QThread::currentThread();
    thisThread->setPriority(QThread::LowPriority);

    ScopedTransaction transaction(database);

    QSqlQuery query(database);
    query.prepare("INSERT INTO " + kRekordboxLibraryTable +
            " (rb_id, artist, title, album, year,"
            "genre,comment,tracknumber,bpm, bitrate,duration, location,"
            "rating,key,analyze_path,device,color) VALUES (:rb_id, :artist, "
            ":title, :album, :year,:genre,"
            ":comment, :tracknumber,:bpm, :bitrate,:duration, :location,"
            ":rating,:key,:analyze_path,:device,:color)");

    int audioFilesCount = 0;

    // Create a playlist for all the tracks on a device
    int playlistID = createDevicePlaylist(database, devicePath);

    QSqlQuery queryInsertIntoDevicePlaylistTracks(database);
    queryInsertIntoDevicePlaylistTracks.prepare(
            "INSERT INTO " + kRekordboxPlaylistTracksTable +
            " (playlist_id, track_id, position) "
            "VALUES (:playlist_id, :track_id, :position)");

    queryInsertIntoDevicePlaylistTracks.bindValue(":playlist_id", playlistID);

    mixxx::FileInfo fileInfo(dbPath);
    if (!Sandbox::askForAccess(&fileInfo)) {
        return QString();
    }
    std::ifstream ifs(dbPath.toStdString(), std::ifstream::binary);
    kaitai::kstream ks(&ifs);

    rekordbox_pdb_t rekordboxDB = rekordbox_pdb_t(&ks);

    // There are other types of tables (eg. COLOR), these are the only ones we are
    // interested at the moment. Perhaps when/if
    // https://github.com/mixxxdj/mixxx/issues/6852
    // is completed, this can be revisited.
    // Attempt was made to also recover HISTORY
    // playlists (which are found on removable Rekordbox devices), however
    // they didn't appear to contain valid row_ref_t structures.
    constexpr int totalTables = 8;

    rekordbox_pdb_t::page_type_t tableOrder[totalTables] = {
            rekordbox_pdb_t::PAGE_TYPE_KEYS,
            rekordbox_pdb_t::PAGE_TYPE_GENRES,
            rekordbox_pdb_t::PAGE_TYPE_ARTISTS,
            rekordbox_pdb_t::PAGE_TYPE_ALBUMS,
            rekordbox_pdb_t::PAGE_TYPE_PLAYLIST_ENTRIES,
            rekordbox_pdb_t::PAGE_TYPE_TRACKS,
            rekordbox_pdb_t::PAGE_TYPE_PLAYLIST_TREE,
            rekordbox_pdb_t::PAGE_TYPE_HISTORY};

    QMap<uint32_t, QString> keysMap;
    QMap<uint32_t, QString> genresMap;
    QMap<uint32_t, QString> artistsMap;
    QMap<uint32_t, QString> albumsMap;
    QMap<uint32_t, QString> playlistNameMap;
    QMap<uint32_t, bool> playlistIsFolderMap;
    QMap<uint32_t, QMap<uint32_t, uint32_t>> playlistTreeMap;
    QMap<uint32_t, QMap<uint32_t, uint32_t>> playlistTrackMap;

    bool folderOrPlaylistFound = false;

    for (int tableOrderIndex = 0; tableOrderIndex < totalTables; tableOrderIndex++) {
        for (const auto& table : *rekordboxDB.tables()) {
            if (table->type() == tableOrder[tableOrderIndex]) {
                uint16_t lastIndex = table->last_page()->index();
                rekordbox_pdb_t::page_ref_t* currentRef = table->first_page();

                while (true) {
                    rekordbox_pdb_t::page_t* page = currentRef->body();

                    if (page->is_data_page()) {
                        for (const auto& rowgroup : *page->row_groups()) {
                            for (const auto& rowRef : *rowgroup->rows()) {
                                if (rowRef->present()) {
                                    switch (tableOrder[tableOrderIndex]) {
                                    case rekordbox_pdb_t::PAGE_TYPE_KEYS: {
                                        auto* key =
                                                static_cast<rekordbox_pdb_t::key_row_t*>(
                                                        rowRef->body());
                                        keysMap[key->id()] = getText(key->name());
                                    } break;
                                    case rekordbox_pdb_t::PAGE_TYPE_GENRES: {
                                        auto* genre =
                                                static_cast<rekordbox_pdb_t::genre_row_t*>(
                                                        rowRef->body());
                                        genresMap[genre->id()] = getText(genre->name());
                                    } break;
                                    case rekordbox_pdb_t::PAGE_TYPE_ARTISTS: {
                                        auto* artist =
                                                static_cast<rekordbox_pdb_t::artist_row_t*>(
                                                        rowRef->body());
                                        artistsMap[artist->id()] = getText(artist->name());
                                    } break;
                                    case rekordbox_pdb_t::PAGE_TYPE_ALBUMS: {
                                        auto* album =
                                                static_cast<rekordbox_pdb_t::album_row_t*>(
                                                        rowRef->body());
                                        albumsMap[album->id()] = getText(album->name());
                                    } break;
                                    case rekordbox_pdb_t::PAGE_TYPE_PLAYLIST_ENTRIES: {
                                        auto* playlistEntry =
                                                static_cast<rekordbox_pdb_t::playlist_entry_row_t*>(
                                                        rowRef->body());
                                        playlistTrackMap
                                                [playlistEntry->playlist_id()]
                                                [playlistEntry->entry_index()] =
                                                        playlistEntry
                                                                ->track_id();
                                    } break;
                                    case rekordbox_pdb_t::PAGE_TYPE_TRACKS: {
                                        insertTrack(database,
                                                static_cast<rekordbox_pdb_t::track_row_t*>(
                                                        rowRef->body()),
                                                query,
                                                queryInsertIntoDevicePlaylistTracks,
                                                artistsMap,
                                                albumsMap,
                                                genresMap,
                                                keysMap,
                                                devicePath,
                                                device,
                                                audioFilesCount);

                                        audioFilesCount++;
                                    } break;
                                    case rekordbox_pdb_t::PAGE_TYPE_PLAYLIST_TREE: {
                                        auto* playlistTree =
                                                static_cast<rekordbox_pdb_t::playlist_tree_row_t*>(
                                                        rowRef->body());

                                        playlistNameMap[playlistTree->id()] =
                                                getText(playlistTree->name());
                                        playlistIsFolderMap[playlistTree
                                                                    ->id()] =
                                                playlistTree->is_folder();
                                        playlistTreeMap
                                                [playlistTree->parent_id()]
                                                [playlistTree->sort_order()] =
                                                        playlistTree->id();

                                        folderOrPlaylistFound = true;
                                    } break;
                                    default:
                                        // we currently don't handle any other
                                        // data, even though there is more.
                                        break;
                                    }
                                }
                            }
                        }
                    }

                    if (currentRef->index() == lastIndex) {
                        break;
                    } else {
                        currentRef = page->next_page();
                    }
                }
            }
        }
    }

    if (audioFilesCount > 0 || folderOrPlaylistFound) {
        // If we have found anything, recursively build playlist/folder TreeItem children
        // for the original device TreeItem
        buildPlaylistTree(database,
                deviceItem,
                0,
                playlistNameMap,
                playlistIsFolderMap,
                playlistTreeMap,
                playlistTrackMap,
                devicePath,
                device);
    }

    qDebug() << "Found: " << audioFilesCount << " audio files in Rekordbox device " << device;

    transaction.commit();

    return devicePath;
}

void buildPlaylistTree(
        QSqlDatabase& database,
        TreeItem* parent,
        uint32_t parentID,
        QMap<uint32_t, QString>& playlistNameMap,
        QMap<uint32_t, bool>& playlistIsFolderMap,
        QMap<uint32_t, QMap<uint32_t, uint32_t>>& playlistTreeMap,
        QMap<uint32_t, QMap<uint32_t, uint32_t>>& playlistTrackMap,
        const QString& playlistPath,
        const QString& device) {
    for (uint32_t childIndex = 0;
            childIndex < (uint32_t)playlistTreeMap[parentID].size();
            childIndex++) {
        uint32_t childID = playlistTreeMap[parentID][childIndex];
        if (childID == 0) {
            continue;
        }
        QString playlistItemName = playlistNameMap[childID];

        QString currentPath = playlistPath + kPLaylistPathDelimiter + playlistItemName;

        TreeItem* child = parent->appendChild(playlistItemName,
                QVariant(QList<QString>{currentPath, IS_NOT_RECORDBOX_DEVICE}));

        // Create a playlist for this child
        QSqlQuery queryInsertIntoPlaylist(database);
        queryInsertIntoPlaylist.prepare(
                "INSERT INTO " + kRekordboxPlaylistsTable +
                " (name) "
                "VALUES (:name)");

        queryInsertIntoPlaylist.bindValue(":name", currentPath);

        if (!queryInsertIntoPlaylist.exec()) {
            LOG_FAILED_QUERY(queryInsertIntoPlaylist)
                    << "currentPath" << currentPath;
            return;
        }

        QSqlQuery idQuery(database);
        idQuery.prepare("select id from " + kRekordboxPlaylistsTable + " where name=:path");
        idQuery.bindValue(":path", currentPath);

        if (!idQuery.exec()) {
            LOG_FAILED_QUERY(idQuery)
                    << "currentPath" << currentPath;
            return;
        }

        int playlistID = kInvalidPlaylistId;
        while (idQuery.next()) {
            playlistID = idQuery.value(idQuery.record().indexOf("id")).toInt();
        }

        QSqlQuery queryInsertIntoPlaylistTracks(database);
        queryInsertIntoPlaylistTracks.prepare(
                "INSERT INTO " + kRekordboxPlaylistTracksTable +
                " (playlist_id, track_id, position) "
                "VALUES (:playlist_id, :track_id, :position)");

        if (playlistTrackMap.contains(childID)) {
            // Add playlist tracks for children
            for (uint32_t trackIndex = 1; trackIndex <=
                    static_cast<uint32_t>(playlistTrackMap[childID].size());
                    trackIndex++) {
                uint32_t rbTrackID = playlistTrackMap[childID][trackIndex];

                int trackID = -1;
                QSqlQuery finderQuery(database);
                finderQuery.prepare("select id from " + kRekordboxLibraryTable +
                        " where rb_id=:rb_id and device=:device");
                finderQuery.bindValue(":rb_id", rbTrackID);
                finderQuery.bindValue(":device", device);

                if (!finderQuery.exec()) {
                    LOG_FAILED_QUERY(finderQuery)
                            << "rbTrackID:" << rbTrackID
                            << "device:" << device;
                    return;
                }

                if (finderQuery.next()) {
                    trackID = finderQuery.value(finderQuery.record().indexOf("id")).toInt();
                }

                queryInsertIntoPlaylistTracks.bindValue(":playlist_id", playlistID);
                queryInsertIntoPlaylistTracks.bindValue(":track_id", trackID);
                queryInsertIntoPlaylistTracks.bindValue(":position", static_cast<int>(trackIndex));

                if (!queryInsertIntoPlaylistTracks.exec()) {
                    LOG_FAILED_QUERY(queryInsertIntoPlaylistTracks)
                            << "playlistID:" << playlistID
                            << "trackID:" << trackID
                            << "trackIndex:" << trackIndex;

                    return;
                }
            }
        }

        if (playlistIsFolderMap[childID]) {
            // If this child is a folder (playlists are only leaf nodes), build playlist tree for it
            buildPlaylistTree(database,
                    child,
                    childID,
                    playlistNameMap,
                    playlistIsFolderMap,
                    playlistTreeMap,
                    playlistTrackMap,
                    currentPath,
                    device);
        }
    }
}

void clearDeviceTables(QSqlDatabase& database, TreeItem* child) {
    ScopedTransaction transaction(database);

    int trackID = -1;
    int playlistID = kInvalidPlaylistId;
    QSqlQuery tracksQuery(database);
    tracksQuery.prepare("select id from " + kRekordboxLibraryTable + " where device=:device");
    tracksQuery.bindValue(":device", child->getLabel());

    QSqlQuery deletePlaylistsQuery(database);
    deletePlaylistsQuery.prepare("delete from " + kRekordboxPlaylistsTable + " where id=:id");

    QSqlQuery deletePlaylistTracksQuery(database);
    deletePlaylistTracksQuery.prepare("delete from " +
            kRekordboxPlaylistTracksTable + " where playlist_id=:playlist_id");

    if (!tracksQuery.exec()) {
        LOG_FAILED_QUERY(tracksQuery)
                << "device:" << child->getLabel();
    }

    while (tracksQuery.next()) {
        trackID = tracksQuery.value(tracksQuery.record().indexOf("id")).toInt();

        QSqlQuery playlistTracksQuery(database);
        playlistTracksQuery.prepare("select playlist_id from " +
                kRekordboxPlaylistTracksTable + " where track_id=:track_id");
        playlistTracksQuery.bindValue(":track_id", trackID);

        if (!playlistTracksQuery.exec()) {
            LOG_FAILED_QUERY(playlistTracksQuery)
                    << "trackID:" << trackID;
        }

        while (playlistTracksQuery.next()) {
            playlistID = playlistTracksQuery
                                 .value(playlistTracksQuery.record().indexOf(
                                         "playlist_id"))
                                 .toInt();

            deletePlaylistsQuery.bindValue(":id", playlistID);

            if (!deletePlaylistsQuery.exec()) {
                LOG_FAILED_QUERY(deletePlaylistsQuery)
                        << "playlistID:" << playlistID;
            }

            deletePlaylistTracksQuery.bindValue(":playlist_id", playlistID);

            if (!deletePlaylistTracksQuery.exec()) {
                LOG_FAILED_QUERY(deletePlaylistTracksQuery)
                        << "playlistID:" << playlistID;
            }
        }
    }

    QSqlQuery deleteTracksQuery(database);
    deleteTracksQuery.prepare("delete from " + kRekordboxLibraryTable + " where device=:device");
    deleteTracksQuery.bindValue(":device", child->getLabel());

    if (!deleteTracksQuery.exec()) {
        LOG_FAILED_QUERY(deleteTracksQuery)
                << "device:" << child->getLabel();
    }

    transaction.commit();
}

void setHotCue(TrackPointer track,
        mixxx::audio::FramePos startPosition,
        mixxx::audio::FramePos endPosition,
        int id,
        const QString& label,
        mixxx::RgbColor::optional_t color) {
    CuePointer pCue;
    const QList<CuePointer> cuePoints = track->getCuePoints();
    for (const CuePointer& trackCue : cuePoints) {
        if (trackCue->getHotCue() == id) {
            pCue = trackCue;
            break;
        }
    }

    mixxx::CueType type = mixxx::CueType::HotCue;
    if (endPosition.isValid()) {
        type = mixxx::CueType::Loop;
    }

    if (pCue) {
        pCue->setStartAndEndPosition(startPosition, endPosition);
    } else {
        pCue = track->createAndAddCue(
                type,
                id,
                startPosition,
                endPosition);
    }
    pCue->setLabel(label);
    if (color) {
        pCue->setColor(*color);
    }
}

void readAnalyze(TrackPointer track,
        mixxx::audio::SampleRate sampleRate,
        int timingOffset,
        bool ignoreCues,
        const QString& anlzPath) {
    if (!QFile(anlzPath).exists()) {
        return;
    }

    qDebug() << "Rekordbox ANLZ path:" << anlzPath << " for: " << track->getTitle();

    std::ifstream ifs(anlzPath.toStdString(), std::ifstream::binary);
    kaitai::kstream ks(&ifs);

    rekordbox_anlz_t anlz = rekordbox_anlz_t(&ks);

    const double sampleRateKhz = sampleRate / 1000.0;

    QList<memory_cue_loop_t> memoryCuesAndLoops;
    int lastHotCueIndex = 0;

    for (const auto& section : *anlz.sections()) {
        switch (section->fourcc()) {
        case rekordbox_anlz_t::SECTION_TAGS_BEAT_GRID: {
            if (!ignoreCues) {
                break;
            }

            auto* beatGridTag =
                    static_cast<rekordbox_anlz_t::beat_grid_tag_t*>(
                            section->body());

            QVector<mixxx::audio::FramePos> beats;

            for (const auto& beat : *beatGridTag->beats()) {
                int time = static_cast<int>(beat->time()) - timingOffset;
                // Ensure no offset times are less than 1
                if (time < 1) {
                    time = 1;
                }
                beats << mixxx::audio::FramePos(sampleRateKhz * static_cast<double>(time));
            }

            const auto pBeats = mixxx::Beats::fromBeatPositions(
                    sampleRate,
                    beats,
                    mixxx::rekordboxconstants::beatsSubversion);
            track->trySetBeats(pBeats);
        } break;
        case rekordbox_anlz_t::SECTION_TAGS_CUES: {
            if (ignoreCues) {
                break;
            }

            auto* cuesTag =
                    static_cast<rekordbox_anlz_t::cue_tag_t*>(
                            section->body());

            for (const auto& cueEntry : *cuesTag->cues()) {
                int time = static_cast<int>(cueEntry->time()) - timingOffset;
                // Ensure no offset times are less than 1
                if (time < 1) {
                    time = 1;
                }
                const auto position = mixxx::audio::FramePos(
                        sampleRateKhz * static_cast<double>(time));

                switch (cuesTag->type()) {
                case rekordbox_anlz_t::CUE_LIST_TYPE_MEMORY_CUES: {
                    switch (cueEntry->type()) {
                    case rekordbox_anlz_t::CUE_ENTRY_TYPE_MEMORY_CUE: {
                        memory_cue_loop_t memoryCue;
                        memoryCue.startPosition = position;
                        memoryCue.endPosition = mixxx::audio::kInvalidFramePos;
                        memoryCue.color = mixxx::RgbColor::nullopt();
                        memoryCuesAndLoops << memoryCue;
                    } break;
                    case rekordbox_anlz_t::CUE_ENTRY_TYPE_LOOP: {
                        int endTime = static_cast<int>(cueEntry->loop_time()) - timingOffset;
                        // Ensure no offset times are less than 1
                        if (endTime < 1) {
                            endTime = 1;
                        }

                        memory_cue_loop_t loop;
                        loop.startPosition = position;
                        loop.endPosition = mixxx::audio::FramePos(
                                sampleRateKhz * static_cast<double>(endTime));
                        loop.color = mixxx::RgbColor::nullopt();
                        memoryCuesAndLoops << loop;
                    } break;
                    }
                } break;
                case rekordbox_anlz_t::CUE_LIST_TYPE_HOT_CUES: {
                    int hotCueIndex = static_cast<int>(cueEntry->hot_cue() - 1);
                    if (hotCueIndex > lastHotCueIndex) {
                        lastHotCueIndex = hotCueIndex;
                    }
                    setHotCue(
                            track,
                            position,
                            mixxx::audio::kInvalidFramePos,
                            hotCueIndex,
                            QString(),
                            mixxx::RgbColor::nullopt());
                } break;
                }
            }
        } break;
        case rekordbox_anlz_t::SECTION_TAGS_CUES_2: {
            if (ignoreCues) {
                break;
            }

            auto* cuesExtendedTag =
                    static_cast<rekordbox_anlz_t::cue_extended_tag_t*>(
                            section->body());

            for (const auto& cueExtendedEntry : *cuesExtendedTag->cues()) {
                int time = static_cast<int>(cueExtendedEntry->time()) - timingOffset;
                // Ensure no offset times are less than 1
                if (time < 1) {
                    time = 1;
                }
                const auto position = mixxx::audio::FramePos(
                        sampleRateKhz * static_cast<double>(time));

                switch (cuesExtendedTag->type()) {
                case rekordbox_anlz_t::CUE_LIST_TYPE_MEMORY_CUES: {
                    switch (cueExtendedEntry->type()) {
                    case rekordbox_anlz_t::CUE_ENTRY_TYPE_MEMORY_CUE: {
                        memory_cue_loop_t memoryCue;
                        memoryCue.startPosition = position;
                        memoryCue.endPosition = mixxx::audio::kInvalidFramePos;
                        memoryCue.comment = fromUtf16BeString(cueExtendedEntry->comment());
                        memoryCue.color = colorFromID(static_cast<int>(
                                cueExtendedEntry->color_id()));
                        memoryCuesAndLoops << memoryCue;
                    } break;
                    case rekordbox_anlz_t::CUE_ENTRY_TYPE_LOOP: {
                        int endTime =
                                static_cast<int>(
                                        cueExtendedEntry->loop_time()) -
                                timingOffset;
                        // Ensure no offset times are less than 1
                        if (endTime < 1) {
                            endTime = 1;
                        }

                        memory_cue_loop_t loop;
                        loop.startPosition = position;
                        loop.endPosition = mixxx::audio::FramePos(
                                sampleRateKhz * static_cast<double>(endTime));
                        loop.comment = fromUtf16BeString(cueExtendedEntry->comment());
                        loop.color = colorFromID(static_cast<int>(cueExtendedEntry->color_id()));
                        memoryCuesAndLoops << loop;
                    } break;
                    }
                } break;
                case rekordbox_anlz_t::CUE_LIST_TYPE_HOT_CUES: {
                    int hotCueIndex = static_cast<int>(cueExtendedEntry->hot_cue() - 1);
                    if (hotCueIndex > lastHotCueIndex) {
                        lastHotCueIndex = hotCueIndex;
                    }
                    setHotCue(track,
                            position,
                            mixxx::audio::kInvalidFramePos,
                            hotCueIndex,
                            fromUtf16BeString(cueExtendedEntry->comment()),
                            mixxx::RgbColor(qRgb(
                                    static_cast<int>(
                                            cueExtendedEntry->color_red()),
                                    static_cast<int>(
                                            cueExtendedEntry->color_green()),
                                    static_cast<int>(cueExtendedEntry
                                                    ->color_blue()))));
                } break;
                }
            }
        } break;
        default:
            break;
        }
    }

    if (memoryCuesAndLoops.size() > 0) {
        std::sort(memoryCuesAndLoops.begin(),
                memoryCuesAndLoops.end(),
                [](const memory_cue_loop_t& a, const memory_cue_loop_t& b)
                        -> bool { return a.startPosition < b.startPosition; });

        bool mainCueFound = false;

        // Add memory cues and loops
        for (int memoryCueOrLoopIndex = 0;
                memoryCueOrLoopIndex < memoryCuesAndLoops.size();
                memoryCueOrLoopIndex++) {
            memory_cue_loop_t memoryCueOrLoop = memoryCuesAndLoops[memoryCueOrLoopIndex];

            if (!mainCueFound && !memoryCueOrLoop.endPosition.isValid()) {
                // Set first chronological memory cue as Mixxx MainCue
                track->setMainCuePosition(memoryCueOrLoop.startPosition);
                CuePointer pMainCue = track->findCueByType(mixxx::CueType::MainCue);
                pMainCue->setLabel(memoryCueOrLoop.comment);
                pMainCue->setColor(*memoryCueOrLoop.color);
                mainCueFound = true;
            } else {
                // Mixxx v2.4 will feature multiple loops, so these saved here will be usable
                // For 2.3, Mixxx treats them as hotcues and the first one will be loaded as the single loop Mixxx supports
                lastHotCueIndex++;
                setHotCue(
                        track,
                        memoryCueOrLoop.startPosition,
                        memoryCueOrLoop.endPosition,
                        lastHotCueIndex,
                        memoryCueOrLoop.comment,
                        memoryCueOrLoop.color);
            }
        }
    }
}

} // anonymous namespace

RekordboxPlaylistModel::RekordboxPlaylistModel(QObject* parent,
        TrackCollectionManager* trackCollectionManager,
        QSharedPointer<BaseTrackCache> trackSource)
        : BaseExternalPlaylistModel(parent,
                  trackCollectionManager,
                  "mixxx.db.model.rekordbox.playlistmodel",
                  kRekordboxPlaylistsTable,
                  kRekordboxPlaylistTracksTable,
                  trackSource) {
}

void RekordboxPlaylistModel::initSortColumnMapping() {
    // Add a bijective mapping between the SortColumnIds and column indices
    for (int i = 0; i < static_cast<int>(TrackModel::SortColumnId::IdMax); ++i) {
        m_columnIndexBySortColumnId[i] = -1;
    }

    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Artist)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_ARTIST);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Title)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_TITLE);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Album)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_ALBUM);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::AlbumArtist)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_ALBUMARTIST);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Year)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_YEAR);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Genre)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_GENRE);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Composer)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_COMPOSER);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Grouping)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_GROUPING);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::TrackNumber)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_TRACKNUMBER);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::FileType)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_FILETYPE);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::NativeLocation)] =
            fieldIndex(ColumnCache::COLUMN_TRACKLOCATIONSTABLE_LOCATION);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Comment)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_COMMENT);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Duration)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_DURATION);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::BitRate)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_BITRATE);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Bpm)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_BPM);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::ReplayGain)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_REPLAYGAIN);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::DateTimeAdded)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_DATETIMEADDED);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::TimesPlayed)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_TIMESPLAYED);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::LastPlayedAt)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_LAST_PLAYED_AT);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Rating)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_RATING);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Key)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_KEY);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Preview)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_PREVIEW);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Color)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_COLOR);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::CoverArt)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_COVERART);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Position)] =
            fieldIndex(ColumnCache::COLUMN_PLAYLISTTRACKSTABLE_POSITION);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::PlaylistDateTimeAdded)] =
            fieldIndex(ColumnCache::COLUMN_PLAYLISTTRACKSTABLE_DATETIMEADDED);

    m_sortColumnIdByColumnIndex.clear();
    for (int i = static_cast<int>(TrackModel::SortColumnId::IdMin);
            i < static_cast<int>(TrackModel::SortColumnId::IdMax);
            ++i) {
        TrackModel::SortColumnId sortColumn = static_cast<TrackModel::SortColumnId>(i);
        m_sortColumnIdByColumnIndex.insert(
                m_columnIndexBySortColumnId[static_cast<int>(sortColumn)],
                sortColumn);
    }
}

TrackPointer RekordboxPlaylistModel::getTrack(const QModelIndex& index) const {
    qDebug() << "RekordboxTrackModel::getTrack";

    TrackPointer track = BaseExternalPlaylistModel::getTrack(index);
    QString location = getFieldVariant(
            index, ColumnCache::COLUMN_TRACKLOCATIONSTABLE_LOCATION)
                               .toString();

    if (!QFile(location).exists()) {
        return track;
    }

    // The following code accounts for timing offsets required to
    // correctly align timing information (cue points, loops, beatgrids)
    // exported from Rekordbox. This is caused by different MP3
    // decoders treating MP3s encoded in a variety of different cases
    // differently. The mp3guessenc library is used to determine which
    // case the MP3 is classified in. See the following PR for more
    // detailed information:
    // https://github.com/mixxxdj/mixxx/pull/2119

    int timingOffset = 0;

    if (location.endsWith(".mp3", Qt::CaseInsensitive)) {
        int timingShiftCase = mp3guessenc_timing_shift_case(location.toStdString().c_str());

        qDebug() << "Timing shift case:" << timingShiftCase << "for MP3 file:" << location;

        switch (timingShiftCase) {
#ifdef __COREAUDIO__
        case EXIT_CODE_CASE_A:
            timingOffset = 12;
            break;
        case EXIT_CODE_CASE_B:
            timingOffset = 13;
            break;
        case EXIT_CODE_CASE_C:
            timingOffset = 26;
            break;
        case EXIT_CODE_CASE_D:
            timingOffset = 50;
            break;
#elif defined(__MAD__)
        case EXIT_CODE_CASE_A:
        case EXIT_CODE_CASE_D:
            timingOffset = 26;
            break;
#elif defined(__FFMPEG__)
        case EXIT_CODE_CASE_D:
            timingOffset = 26;
            break;
#endif
        }
    }

#ifdef __COREAUDIO__
    if (location.toLower().endsWith(".m4a")) {
        timingOffset = 48;
    }
#endif

    mixxx::audio::SampleRate sampleRate = track->getSampleRate();

    QString anlzPath =
            getFieldVariant(index, ColumnCache::COLUMN_REKORDBOX_ANALYZE_PATH)
                    .toString();
    QString anlzPathExt = anlzPath.left(anlzPath.length() - 3) + "EXT";

    if (QFile(anlzPathExt).exists()) {
        // Beatgrids appear to be only correct in legacy ANLZ file
        readAnalyze(track, sampleRate, timingOffset, true, anlzPath);
        readAnalyze(track, sampleRate, timingOffset, false, anlzPathExt);
    } else {
        readAnalyze(track, sampleRate, timingOffset, false, anlzPath);
    }

    // Assume that the key of the file the has been analyzed in Recordbox is correct
    // and prevent the AnalyzerKey from re-analyzing.
    // Form 5.4.3 Key format depends on the preferences option:
    // Classic: Abm,B,Ebm,F#,Bbm,Db,Fm,Ab,…,F#m,A,Dbm,E
    // Alphanumeric (Camelot): 1A,1B,2A,2B,3A,3B,4A,4B,…,11A,11B,12A,12B
    // Not reckognized: 1m, 01A
    // Earlier versions allow any format
    // Decision: We normalize the KeyText here to not write garbage to the
    // file metadata and it is unlikely to loose extra info.
    track->setKeys(KeyFactory::makeBasicKeysNormalized(
            getFieldVariant(index, ColumnCache::COLUMN_LIBRARYTABLE_KEY).toString(),
            mixxx::track::io::key::USER));

    track->setColor(mixxx::RgbColor::fromQVariant(
            getFieldVariant(index, ColumnCache::COLUMN_LIBRARYTABLE_COLOR)));

    return track;
}

bool RekordboxPlaylistModel::isColumnHiddenByDefault(int column) {
    if (column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_BITRATE)) {
        return true;
    }
    return BaseSqlTableModel::isColumnHiddenByDefault(column);
}

bool RekordboxPlaylistModel::isColumnInternal(int column) {
    return column == fieldIndex(ColumnCache::COLUMN_REKORDBOX_ANALYZE_PATH) ||
            BaseExternalPlaylistModel::isColumnInternal(column);
}

RekordboxFeature::RekordboxFeature(
        Library* pLibrary,
        UserSettingsPointer pConfig)
        : BaseExternalLibraryFeature(pLibrary, pConfig, QStringLiteral("rekordbox")),
          m_pSidebarModel(make_parented<TreeItemModel>(this)) {
    QString tableName = kRekordboxLibraryTable;
    QString idColumn = LIBRARYTABLE_ID;
    QStringList columns = {
            LIBRARYTABLE_ID,
            LIBRARYTABLE_ARTIST,
            LIBRARYTABLE_TITLE,
            LIBRARYTABLE_ALBUM,
            LIBRARYTABLE_YEAR,
            LIBRARYTABLE_GENRE,
            LIBRARYTABLE_TRACKNUMBER,
            TRACKLOCATIONSTABLE_LOCATION,
            LIBRARYTABLE_COMMENT,
            LIBRARYTABLE_RATING,
            LIBRARYTABLE_DURATION,
            LIBRARYTABLE_BITRATE,
            LIBRARYTABLE_BPM,
            LIBRARYTABLE_KEY,
            LIBRARYTABLE_COLOR,
            REKORDBOX_ANALYZE_PATH};

    const QStringList searchColumns = {
            LIBRARYTABLE_ARTIST,
            LIBRARYTABLE_TITLE,
            LIBRARYTABLE_ALBUM,
            LIBRARYTABLE_GENRE,
            LIBRARYTABLE_TRACKNUMBER,
            TRACKLOCATIONSTABLE_LOCATION,
            LIBRARYTABLE_COMMENT};

    m_trackSource = QSharedPointer<BaseTrackCache>::create(
            m_pTrackCollection,
            tableName,
            std::move(idColumn),
            std::move(columns),
            std::move(searchColumns),
            false);
    m_pRekordboxPlaylistModel = make_parented<RekordboxPlaylistModel>(
            this, pLibrary->trackCollectionManager(), m_trackSource);

    m_title = tr("Rekordbox");

    QSqlDatabase database = m_pTrackCollection->database();
    ScopedTransaction transaction(database);
    // Drop any leftover temporary Rekordbox database tables if they exist
    dropTable(database, kRekordboxPlaylistTracksTable);
    dropTable(database, kRekordboxPlaylistsTable);
    dropTable(database, kRekordboxLibraryTable);

    // Create new temporary Rekordbox database tables
    createLibraryTable(database, kRekordboxLibraryTable);
    createPlaylistsTable(database, kRekordboxPlaylistsTable);
    createPlaylistTracksTable(database, kRekordboxPlaylistTracksTable);
    transaction.commit();

    connect(&m_devicesFutureWatcher,
            &QFutureWatcher<QList<TreeItem*>>::finished,
            this,
            &RekordboxFeature::onRekordboxDevicesFound);
    connect(&m_tracksFutureWatcher,
            &QFutureWatcher<QString>::finished,
            this,
            &RekordboxFeature::onTracksFound);
    // initialize the model
    m_pSidebarModel->setRootItem(TreeItem::newRoot(this));
}

RekordboxFeature::~RekordboxFeature() {
    m_devicesFuture.waitForFinished();
    m_tracksFuture.waitForFinished();

    // Drop temporary Rekordbox database tables on shutdown
    QSqlDatabase database = m_pTrackCollection->database();
    ScopedTransaction transaction(database);
    dropTable(database, kRekordboxPlaylistTracksTable);
    dropTable(database, kRekordboxPlaylistsTable);
    dropTable(database, kRekordboxLibraryTable);
    transaction.commit();
}

void RekordboxFeature::bindLibraryWidget(WLibrary* pLibraryWidget,
        KeyboardEventFilter* keyboard) {
    Q_UNUSED(keyboard);
    parented_ptr<WLibraryTextBrowser> pEdit = make_parented<WLibraryTextBrowser>(pLibraryWidget);
    pEdit->setHtml(formatRootViewHtml());
    pEdit->setOpenLinks(false);
    connect(pEdit, &WLibraryTextBrowser::anchorClicked, this, &RekordboxFeature::htmlLinkClicked);
    pLibraryWidget->registerView("REKORDBOXHOME", pEdit);
}

void RekordboxFeature::htmlLinkClicked(const QUrl& link) {
    if (QString(link.path()) == "refresh") {
        activate();
    } else {
        qDebug() << "Unknown link clicked" << link;
    }
}

std::unique_ptr<BaseSqlTableModel>
RekordboxFeature::createPlaylistModelForPlaylist(const QString& playlist) {
    auto pModel = std::make_unique<RekordboxPlaylistModel>(
            this, m_pLibrary->trackCollectionManager(), m_trackSource);
    pModel->setPlaylist(playlist);
    return pModel;
}

QVariant RekordboxFeature::title() {
    return m_title;
}

bool RekordboxFeature::isSupported() {
    return true;
}

TreeItemModel* RekordboxFeature::sidebarModel() const {
    return m_pSidebarModel;
}

QString RekordboxFeature::formatRootViewHtml() const {
    QString title = tr("Rekordbox");
    QString summary = tr(
            "Reads databases exported for Pioneer CDJ / XDJ players using "
            "the Rekordbox Export mode.<br/>"
            "Rekordbox can only export to USB or SD devices with a FAT or "
            "HFS file system.<br/>"
            "Mixxx can read a database from any device that contains the "
            "database folders (<tt>PIONEER</tt> and <tt>Contents</tt>).<br/>"
            "Not supported are Rekordbox databases that have been moved to "
            "an external device via<br/>"
            "<i>Preferences > Advanced > Database management</i>.<br/>"
            "<br/>"
            "The following data is read:");

    QStringList items;

    items
            << tr("Folders")
            << tr("Playlists")
            << tr("Beatgrids")
            << tr("Hot cues")
            << tr("Memory cues")
            << tr("Loops (only the first loop is currently usable in Mixxx)");

    QString html;
    QString refreshLink = tr("Check for attached Rekordbox USB / SD devices (refresh)");
    html.append(QString("<h2>%1</h2>").arg(title));
    html.append(QString("<p>%1</p>").arg(summary));
    html.append(QString("<ul>"));
    for (const auto& item : std::as_const(items)) {
        html.append(QString("<li>%1</li>").arg(item));
    }
    html.append(QString("</ul>"));

    //Colorize links in lighter blue, instead of QT default dark blue.
    //Links are still different from regular text, but readable on dark/light backgrounds.
    //https://github.com/mixxxdj/mixxx/issues/9103
    html.append(QString("<a style=\"color:#0496FF;\" href=\"refresh\">%1</a>")
                        .arg(refreshLink));
    return html;
}

void RekordboxFeature::refreshLibraryModels() {
}

void RekordboxFeature::activate() {
    qDebug() << "RekordboxFeature::activate()";

    // Let a worker thread do the XML parsing
    m_devicesFuture = QtConcurrent::run(findRekordboxDevices);
    m_devicesFutureWatcher.setFuture(m_devicesFuture);
    m_title = tr("(loading) Rekordbox");
    //calls a slot in the sidebar model such that 'Rekordbox (isLoading)' is displayed.
    emit featureIsLoading(this, true);

    emit enableCoverArtDisplay(true);
    emit switchToView("REKORDBOXHOME");
    emit disableSearch();
}

void RekordboxFeature::activateChild(const QModelIndex& index) {
    if (!index.isValid()) {
        return;
    }

    //access underlying TreeItem object
    TreeItem* item = static_cast<TreeItem*>(index.internalPointer());
    if (!(item && item->getData().isValid())) {
        return;
    }

    // TreeItem list data holds 2 values in a QList and have different meanings.
    // If the 2nd QList element IS_RECORDBOX_DEVICE, the 1st element is the
    // filesystem device path, and the parseDeviceDB concurrent thread to parse
    // the Rekcordbox database is initiated. If the 2nd element is
    // IS_NOT_RECORDBOX_DEVICE, the 1st element is the playlist path and it is
    // activated.
    QList<QVariant> data = item->getData().toList();
    QString playlist = data[0].toString();
    bool doParseDeviceDB = data[1].toString() == IS_RECORDBOX_DEVICE;

    qDebug() << "RekordboxFeature::activateChild " << item->getLabel()
             << " playlist: " << playlist << " doParseDeviceDB: " << doParseDeviceDB;

    if (doParseDeviceDB) {
        qDebug() << "Parse Rekordbox Device DB: " << playlist;

        // Let a worker thread do the XML parsing
        m_tracksFuture = QtConcurrent::run(parseDeviceDB, static_cast<Library*>(parent())->dbConnectionPool(), item);
        m_tracksFutureWatcher.setFuture(m_tracksFuture);

        // This device is now a playlist element, future activations should treat is
        // as such
        data[1] = QVariant(IS_NOT_RECORDBOX_DEVICE);
        item->setData(QVariant(data));
    } else {
        qDebug() << "Activate Rekordbox Playlist: " << playlist;
        m_pRekordboxPlaylistModel->setPlaylist(playlist);
        emit showTrackModel(m_pRekordboxPlaylistModel);
    }
}

void RekordboxFeature::onRekordboxDevicesFound() {
    const QList<TreeItem*> result = m_devicesFuture.result();
    auto foundDevices = std::vector<std::unique_ptr<TreeItem>>(result.cbegin(), result.cend());

    clearLastRightClickedIndex();

    TreeItem* root = m_pSidebarModel->getRootItem();
    QSqlDatabase database = m_pTrackCollection->database();

    if (foundDevices.size() == 0) {
        // No Rekordbox devices found
        ScopedTransaction transaction(database);

        dropTable(database, kRekordboxPlaylistTracksTable);
        dropTable(database, kRekordboxPlaylistsTable);
        dropTable(database, kRekordboxLibraryTable);

        // Create new temporary Rekordbox database tables
        createLibraryTable(database, kRekordboxLibraryTable);
        createPlaylistsTable(database, kRekordboxPlaylistsTable);
        createPlaylistTracksTable(database, kRekordboxPlaylistTracksTable);

        transaction.commit();

        if (root->childRows() > 0) {
            // Devices have since been unmounted
            m_pSidebarModel->removeRows(0, root->childRows());
        }
    } else {
        for (int deviceIndex = 0; deviceIndex < root->childRows(); deviceIndex++) {
            TreeItem* child = root->child(deviceIndex);
            bool removeChild = true;

            for (const auto& pDeviceFound : foundDevices) {
                if (pDeviceFound->getLabel() == child->getLabel()) {
                    removeChild = false;
                    break;
                }
            }

            if (removeChild) {
                // Device has since been unmounted, cleanup DB
                clearDeviceTables(database, child);

                m_pSidebarModel->removeRows(deviceIndex, 1);
            }
        }

        std::vector<std::unique_ptr<TreeItem>> childrenToAdd;

        for (auto&& pDeviceFound : foundDevices) {
            bool addNewChild = true;
            for (int deviceIndex = 0; deviceIndex < root->childRows(); deviceIndex++) {
                TreeItem* child = root->child(deviceIndex);

                if (pDeviceFound->getLabel() == child->getLabel()) {
                    // This device already exists in the TreeModel, don't add or parse is again
                    addNewChild = false;
                }
            }

            if (addNewChild) {
                childrenToAdd.push_back(std::move(pDeviceFound));
            }
        }

        if (!childrenToAdd.empty()) {
            m_pSidebarModel->insertTreeItemRows(std::move(childrenToAdd), 0);
        }
    }

    // calls a slot in the sidebarmodel such that 'isLoading' is removed from the feature title.
    m_title = tr("Rekordbox");
    emit featureLoadingFinished(this);
}

void RekordboxFeature::onTracksFound() {
    qDebug() << "onTracksFound";
    m_pSidebarModel->triggerRepaint();

    QString devicePlaylist;
    try {
        devicePlaylist = m_tracksFuture.result();
    } catch (const std::exception& e) {
        qWarning() << "Failed to load Rekordbox database:" << e.what();
        return;
    }

    qDebug() << "Show Rekordbox Device Playlist: " << devicePlaylist;

    m_pRekordboxPlaylistModel->setPlaylist(devicePlaylist);
    emit showTrackModel(m_pRekordboxPlaylistModel);
}
