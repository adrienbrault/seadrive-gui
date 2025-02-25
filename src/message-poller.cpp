#include <QTimer>
#include <QDateTime>
#include <QRegularExpression>

#include "utils/utils.h"
#include "utils/translate-commit-desc.h"
#include "utils/json-utils.h"
#include "utils/file-utils.h"
#include "seadrive-gui.h"
#include "daemon-mgr.h"
#include "settings-mgr.h"
#include "rpc/rpc-client.h"
#include "rpc/sync-error.h"
#include "ui/tray-icon.h"
#include "account.h"
#include "account-mgr.h"

#include "message-poller.h"
#if defined(Q_OS_MAC)
#include "sync-command.h"
#endif

namespace {

const int kCheckNotificationIntervalMSecs = 1000;

struct GlobalSyncStatus {
    bool is_syncing;
    qint64 sent_bytes;
    qint64 recv_bytes;

    static GlobalSyncStatus fromJson(const json_t* json);
};

} // namespace

class SeaDriveEvent {
public:
    enum FsOpError {
        UNKNOWN_ERROR = 0,
        CREATE_ROOT_FILE,
        REMOVE_REPO,
    };

    FsOpError fs_op_error;
    QString path, type;

    static SeaDriveEvent fromJson(json_t * root) {
        // char *s = json_dumps(root, 0);
        // printf ("[%s] %s\n", QDateTime::currentDateTime().toString().toUtf8().data(), s);
        // free (s);

        SeaDriveEvent event;
        Json json(root);

        QString type = json.getString("type");
        if (type == "fs_op_error.create_root_file") {
            event.fs_op_error = CREATE_ROOT_FILE;
        } else if (type == "fs_op_error.remove_repo") {
            event.fs_op_error = REMOVE_REPO;
        } else {
            qWarning("unknown type of seadrive event %s", toCStr(type));
            event.fs_op_error = UNKNOWN_ERROR;
        }
        event.path = json.getString("path");
        event.type = type;

        return event;
    }
};


MessagePoller::MessagePoller(QObject *parent): QObject(parent)
{
    check_notification_timer_ = new QTimer(this);
#if defined(Q_OS_MAC)
    sync_command_ = new SyncCommand();
#endif
    connect(check_notification_timer_, SIGNAL(timeout()), this, SLOT(checkSeaDriveEvents()));
    connect(check_notification_timer_, SIGNAL(timeout()), this, SLOT(checkNotification()));
    connect(check_notification_timer_, SIGNAL(timeout()), this, SLOT(checkSyncStatus()));
    connect(check_notification_timer_, SIGNAL(timeout()), this, SLOT(checkSyncErrors()));
}

MessagePoller::~MessagePoller()
{
#if defined(Q_OS_MAC)
    delete sync_command_;
#endif
}

void MessagePoller::start()
{
    check_notification_timer_->start(kCheckNotificationIntervalMSecs);
#if defined(Q_OS_WIN32)
    connect(gui->daemonManager(), SIGNAL(daemonDead()), this, SLOT(onDaemonDead()));
    connect(gui->daemonManager(), SIGNAL(daemonRestarted()), this, SLOT(onDaemonRestarted()));
#endif
}

void MessagePoller::setRpcClient(SeafileRpcClient *rpc_client)
{
    rpc_client_ = rpc_client;
}

void MessagePoller::onDaemonDead()
{
    qDebug("pausing message poller when daemon is dead");
    check_notification_timer_->stop();
}

void MessagePoller::onDaemonRestarted()
{
    check_notification_timer_->start(kCheckNotificationIntervalMSecs);
}

void MessagePoller::checkSeaDriveEvents()
{
    json_t *ret;
    if (!rpc_client_->isConnected()) {
        return;
    }
    if (!rpc_client_->getSeaDriveEvents(&ret)) {
        return;
    }
    SeaDriveEvent event = SeaDriveEvent::fromJson(ret);
    json_decref(ret);

    processSeaDriveEvent(event);
}

void MessagePoller::checkNotification()
{
    json_t *ret;
    if (!rpc_client_->isConnected()) {
        return;
    }
    if (!rpc_client_->getSyncNotification(&ret)) {
        return;
    }
    SyncNotification notification = SyncNotification::fromJson(ret);
    json_decref(ret);

    processNotification(notification);
}

void MessagePoller::checkSyncStatus()
{
    json_t *ret;
    if (!rpc_client_->isConnected()) {
        return;
    }
    if (!rpc_client_->getGlobalSyncStatus(&ret)) {
        return;
    }
    GlobalSyncStatus sync_status = GlobalSyncStatus::fromJson(ret);
    json_decref(ret);

    if (sync_status.is_syncing) {
        gui->trayIcon()->rotate(true);
        gui->trayIcon()->setTransferRate(sync_status.sent_bytes, sync_status.recv_bytes);
    } else {
        gui->trayIcon()->rotate(false);
        gui->trayIcon()->setTransferRate(0, 0);
    }
}

void MessagePoller::checkSyncErrors()
{
    json_t *ret;
    if (!rpc_client_->isConnected()) {
        return;
    }
    if (!rpc_client_->getSyncErrors(&ret)) {
        gui->trayIcon()->setSyncErrors(QList<SyncError>());
        return;
    }

    QList<SyncError> errors = SyncError::listFromJSON(ret);
    json_decref(ret);

    gui->trayIcon()->setSyncErrors(errors);
}

void MessagePoller::processNotification(const SyncNotification& notification)
{
    if (notification.type == "sync.done") {
        if (!gui->settingsManager()->notify()) {
            return;
        }
        QString title = tr("\"%1\" is synchronized").arg(notification.repo_name);
        gui->trayIcon()->showMessage(
            title,
            translateCommitDesc(notification.commit_desc),
            notification.repo_id,
            notification.commit_id,
            notification.parent_commit_id);
    } else if (notification.type == "sync.error") {
#if defined(Q_OS_MAC)
        if (notification.error_id == SYNC_ERROR_ID_INVALID_PATH_ON_WINDOWS &&
            gui->settingsManager()->getHideWindowsIncompatibilityPathMsg()) {
            return;
        }
#endif
        QString path_in_title;
        if (!notification.repo_name.isEmpty()) {
            path_in_title = notification.repo_name;
        } else if (!notification.error_path.isEmpty()) {
            path_in_title = ::getBaseName(notification.error_path);
        }
        QString title;
        if (!path_in_title.isEmpty()) {
            title = tr("Error when syncing \"%1\"").arg(path_in_title);
        } else {
            title = tr("Error when syncing");
        }
        gui->trayIcon()->showMessage(
            title,
            notification.error,
            notification.repo_id,
            "",
            "",
            QSystemTrayIcon::Warning);
    } else if (notification.type == "sync.multipart_upload") {
        if (!gui->settingsManager()->notify()) {
            return;
        }
        QString title = tr("\"%1\" is being uploaded").arg(notification.repo_name);
        gui->trayIcon()->showMessage(
            title,
            translateCommitDesc(notification.commit_desc),
            notification.repo_id,
            notification.commit_id,
            notification.parent_commit_id);
    } else if (notification.type == "fs-loaded") {
        QString title = tr("Libraries are ready");
        QString msg = tr("All libraries are loaded and ready to use.");
        gui->trayIcon()->showMessage(
            title,
            msg,
            "",
            "",
            "",
            QSystemTrayIcon::Information);
        emit seadriveFSLoaded();
    } else if (notification.isCrossRepoMove()) {
        // printf("src path = %s, dst path = %s\n", toCStr(notification.move.src_path), toCStr(notification.move.dst_path));
        QString src = ::getBaseName(notification.move.src_path);
        QString dst = ::getParentPath(notification.move.dst_path) + "/";
        QString title, msg;

        if (notification.move.type == "start") {
            title = tr("Starting to move \"%1\"").arg(src);
            msg = tr("Starting to move \"%1\" to \"%2\"").arg(src, dst);
        } else if (notification.move.type == "done") {
            title = tr("Successfully moved \"%1\"").arg(src);
            msg = tr("Successfully moved \"%1\" to \"%2\"").arg(src, dst);
        } else if (notification.move.type == "error") {
            title = tr("Failed to move \"%1\"").arg(src);
            msg = tr("Failed to move \"%1\" to \"%2\"").arg(src, dst);
        }

        gui->trayIcon()->showMessage(
            title,
            msg,
            "",
            "",
            "",
            QSystemTrayIcon::Information);
    } else if (notification.type == "del_confirmation") {
        QString text;
        QRegularExpression re("Deleted \"(.+)\" and (.+) more files.");
        auto match = re.match(notification.delete_files.trimmed());
        if (match.hasMatch()) {
            text = tr("Deleted \"%1\" and %2 more files.")
                      .arg(match.captured(1)).arg(match.captured(2));
        }

        QString info = tr("Do you want to delete files in library \"%1\" ?")
                          .arg(notification.repo_name.trimmed());

        if (gui->deletingConfirmationBox(text, info)) {
            rpc_client_->addDelConfirmation(notification.confirmation_id, false);
        } else {
            rpc_client_->addDelConfirmation(notification.confirmation_id, true);
        }
    } else if (notification.type == "del_repo_confirmation") {
        QString text;
        text = tr("Deleted library \"%1\"").arg(notification.repo_name.trimmed());

        QString info = tr("Confirm to delete library \"%1\" ?")
                          .arg(notification.repo_name.trimmed());

        if (gui->deletingConfirmationBox(text, info)) {
            rpc_client_->addDelConfirmation(notification.confirmation_id, false);
        } else {
            rpc_client_->addDelConfirmation(notification.confirmation_id, true);
        }
    } else if (notification.type == "action.get_share_link") {
#if defined(Q_OS_MAC)
        Account account = gui->accountManager()->getAccountByDomainID(notification.domain_id);
        if (!account.isValid()) {
            return;
        }
        sync_command_->doShareLink(account, notification.repo_id, notification.repo_path);
#endif
    } else if (notification.type == "action.get_internal_link") {
#if defined(Q_OS_MAC)
        Account account = gui->accountManager()->getAccountByDomainID(notification.domain_id);
        if (!account.isValid()) {
            return;
        }
        sync_command_->doInternalLink(account, notification.repo_id, notification.repo_path, notification.is_dir);
#endif
    } else if (notification.type == "action.get_upload_link") {
#if defined(Q_OS_MAC)
        Account account = gui->accountManager()->getAccountByDomainID(notification.domain_id);
        if (!account.isValid()) {
            return;
        }
        sync_command_->doGetUploadLink(account, notification.repo_id, notification.repo_path);
#endif
    } else if (notification.type == "action.view_file_history") {
#if defined(Q_OS_MAC)
        Account account = gui->accountManager()->getAccountByDomainID(notification.domain_id);
        if (!account.isValid()) {
            return;
        }
        sync_command_->doShowFileHistory(account, notification.repo_id, notification.repo_path);
#endif
    } else {
        qWarning ("Unknown message %s\n", notification.type.toUtf8().data());
    }
}

void MessagePoller::processSeaDriveEvent(const SeaDriveEvent &event)
{
    last_event_path_ = event.path;
    if(event.type == "file-download.start") {
        QString title = tr("Download file");
        QString msg = tr("Start to download file \"%1\" ").arg(::getBaseName(event.path));
        gui->trayIcon()->showMessage(title, msg);
        last_event_type_ = event.type;
        return;
    } else if (event.type == "file-download.done") {
        QString title = tr("Download file");
        QString msg = tr("file \"%1\" has been downloaded ").arg(::getBaseName(event.path));
        gui->trayIcon()->showMessage(title, msg);
        last_event_type_ = event.type;
        return;
    }

    switch (event.fs_op_error) {
        case SeaDriveEvent::CREATE_ROOT_FILE: {
            QString title = tr("Failed to create file \"%1\"").arg(::getBaseName(event.path));
            QString msg = tr("You can't create files in the mount folder directly");
            gui->trayIcon()->showWarningMessage(title, msg);
        } break;
        case SeaDriveEvent::REMOVE_REPO: {
            QString title = tr("Failed to delete folder");
            QString msg = tr("You can't delete the library \"%1\" directly").arg(::getBaseName(event.path));
            gui->trayIcon()->showWarningMessage(title, msg);
        } break;
    default:
        break;
    }
}

SyncNotification SyncNotification::fromJson(const json_t *root)
{
    SyncNotification notification;
    Json json(root);

    // char *s = json_dumps(root, 0);
    // printf ("[%s] %s\n", QDateTime::currentDateTime().toString().toUtf8().data(), s);
    // qWarning ("[%s] %s\n", QDateTime::currentDateTime().toString().toUtf8().data(), s);
    // free (s);

    notification.type = json.getString("type");

    if (notification.type.startsWith("cross-repo-move.")) {
        notification.move.src_path = json.getString("srcpath");
        notification.move.dst_path = json.getString("dstpath");
        notification.move.type = notification.type.split(".").last();
    } else if (notification.type == "del_confirmation") {
        notification.repo_name = json.getString("repo_name");
        notification.confirmation_id = json.getString("confirmation_id");
        notification.delete_files = json.getString("delete_files");
    } else if (notification.type == "del_repo_confirmation") {
        notification.repo_name = json.getString("repo_name");
        notification.confirmation_id = json.getString("confirmation_id");
    } else if (notification.type == "action.get_share_link" ||
               notification.type == "action.get_internal_link" ||
               notification.type == "action.get_upload_link" ||
               notification.type == "action.view_file_history") {
        notification.repo_id = json.getString("repo_id");
        notification.repo_path = json.getString("repo_path");
        notification.domain_id = json.getString("domain_id");
        notification.is_dir = json.getBool("is_dir");
    } else {
        notification.repo_id = json.getString("repo_id");
        notification.repo_name = json.getString("repo_name");
        notification.commit_id = json.getString("commit_id");
        notification.parent_commit_id = json.getString("parent_commit_id");
        notification.commit_desc = json.getString("commit_desc");
        if (notification.isSyncError()) {
            notification.error_id = json.getLong("err_id");
            notification.error_path = json.getString("path");
            notification.error = SyncError::syncErrorIdToErrorStr(notification.error_id, notification.error_path);
        }
    }

    return notification;
}


GlobalSyncStatus GlobalSyncStatus::fromJson(const json_t *root)
{
    GlobalSyncStatus sync_status;
    Json json(root);

    sync_status.is_syncing = json.getLong("is_syncing");
    sync_status.sent_bytes = json.getLong("sent_bytes");
    sync_status.recv_bytes = json.getLong("recv_bytes");

    // char *s = json_dumps(root, 0);
    // printf ("[%s] %s\n", QDateTime::currentDateTime().toString().toUtf8().data(), s);
    // free (s);

    return sync_status;
}
