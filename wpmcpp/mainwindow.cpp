#include <qabstractitemview.h>
#include <qmessagebox.h>
#include <qvariant.h>
#include <qprogressdialog.h>
#include <qwaitcondition.h>
#include <qthread.h>
#include <windows.h>
#include <qtimer.h>
#include <qdebug.h>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QRegExp>
#include <qdatetime.h>
#include "qdesktopservices.h"
#include <qinputdialog.h>

#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "repository.h"
#include "job.h"
#include "progressdialog.h"
#include "settingsdialog.h"
#include "wpmutils.h"
#include "installoperation.h"

class InstallThread: public QThread
{
    PackageVersion* pv;

    // 0, 1, 2 = install/uninstall
    // 3, 4 = recognize installed applications + load repositories
    // 5 = download the package and compute it's SHA1
    int type;

    Job* job;
public:
    /** computed SHA1 will be stored here (type == 5) */
    QString sha1;

    QList<InstallOperation*> install;

    InstallThread(PackageVersion* pv, int type, Job* job);

    void run();
};

InstallThread::InstallThread(PackageVersion *pv, int type, Job* job)
{
    this->pv = pv;
    this->type = type;
    this->job = job;
}

void InstallThread::run()
{
    CoInitialize(NULL);

    // qDebug() << "InstallThread::run.1";
    switch (this->type) {
    case 0:
    case 1:
    case 2: {
        job->setCancellable(true);

        int n = install.count();

        for (int i = 0; i < this->install.count(); i++) {
            InstallOperation* op = install.at(i);
            PackageVersion* pv = op->packageVersion;
            if (op->install)
                job->setHint(QString("Installing %1").arg(
                        pv->toString()));
            else
                job->setHint(QString("Uninstalling %1").arg(
                        pv->toString()));
            Job* sub = job->newSubJob(1.0 / n);
            if (op->install)
                pv->install(sub);
            else
                pv->uninstall(sub);
            delete sub;

            if (!job->getErrorMessage().isEmpty())
                break;
        }

        job->complete();
        break;
    }
    case 3:
    case 4:
        Repository::getDefault()->load(job);
        break;
    case 5:
        this->sha1 = pv->downloadAndComputeSHA1(job);
        break;
    }

    CoUninitialize();

    // qDebug() << "InstallThread::run.2";
}

bool MainWindow::winEvent(MSG* message, long* result)
{    
    if (message->message == WM_ICONTRAY) {
        // qDebug() << "MainWindow::winEvent " << message->lParam;
        switch (message->lParam) {
            case (LPARAM) NIN_BALLOONUSERCLICK:
                this->ui->comboBoxStatus->setCurrentIndex(3);
                this->prepare();
                ((QApplication*) QApplication::instance())->
                        setQuitOnLastWindowClosed(true);
                this->showMaximized();
                this->activateWindow();
                break;
            case (LPARAM) NIN_BALLOONTIMEOUT:
                ((QApplication*) QApplication::instance())->quit();
                break;
        }
        return true;
    }
    return false;
}

void MainWindow::updateIcons()
{
    Repository* r = Repository::getDefault();
    for (int i = 0; i < this->ui->tableWidget->rowCount(); i++) {
        QTableWidgetItem *item = ui->tableWidget->item(i, 0);
        const QVariant v = item->data(Qt::UserRole);
        PackageVersion* pv = (PackageVersion *) v.value<void*>();
        Package* p = r->findPackage(pv->package);

        if (p) {
            if (!p->icon.isEmpty() && this->icons.contains(p->icon)) {
                QIcon icon = this->icons[p->icon];
                item->setIcon(icon);
            }
        }
    }
}

MainWindow::MainWindow(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    setWindowTitle("Npackd");

    this->ui->tableWidget->setEditTriggers(QTableWidget::NoEditTriggers);

    QList<QUrl*> urls = Repository::getRepositoryURLs();
    if (urls.count() == 0) {
        urls.append(new QUrl(
                "http://windows-package-manager.googlecode.com/hg/repository/Rep.xml"));
        Repository::setRepositoryURLs(urls);
    }
    qDeleteAll(urls);
    urls.clear();

    this->on_tableWidget_itemSelectionChanged();
    this->ui->tableWidget->setColumnCount(5);
    this->ui->tableWidget->setColumnWidth(0, 40);
    this->ui->tableWidget->setColumnWidth(1, 150);
    this->ui->tableWidget->setColumnWidth(2, 300);
    this->ui->tableWidget->setIconSize(QSize(32, 32));
    this->ui->tableWidget->sortItems(1);

    connect(&this->fileLoader, SIGNAL(downloaded(const FileLoaderItem&)), this,
            SLOT(iconDownloaded(const FileLoaderItem&)),
            Qt::QueuedConnection);
    this->fileLoader.start(QThread::LowestPriority);
}

void MainWindow::iconDownloaded(const FileLoaderItem& it)
{
    if (it.f) {
        // qDebug() << "MainWindow::iconDownloaded.2 " << it.url;
        QPixmap pm(it.f->fileName());
        delete it.f;
        if (!pm.isNull()) {
            QIcon icon(pm);
            icon.detach();
            this->icons.insert(it.url, icon);
            updateIcons();
        }
    }
}

void MainWindow::prepare()
{
    QTimer *pTimer = new QTimer(this);
    pTimer->setSingleShot(true);
    connect(pTimer, SIGNAL(timeout()), this,
            SLOT(onShow()));

    pTimer->start(0);
}

MainWindow::~MainWindow()
{
    this->fileLoader.terminated = 1;
    if (!this->fileLoader.wait(1000))
        this->fileLoader.terminate();
    delete ui;
}

bool MainWindow::waitFor(Job* job, const QString& title)
{
    ProgressDialog* pd;
    pd = new ProgressDialog(this, job, title);
    pd->setModal(true);

    // qDebug() << "MainWindow::waitFor.1";

    pd->exec();
    delete pd;
    pd = 0;

    // qDebug() << "MainWindow::waitFor.2";
    if (job->isCancelled())
        return false;
    else if (!job->getErrorMessage().isEmpty()) {
        QMessageBox::critical(this,
                "Error", QString("%1: %2").
                arg(job->getHint()).
                arg(job->getErrorMessage()),
                QMessageBox::Ok);
        return false;
    } else {
        return true;
    }
}

void MainWindow::onShow()
{
    recognizeAndloadRepositories();
}

void MainWindow::selectPackageVersion(PackageVersion* pv)
{
    for (int i = 0; i < this->ui->tableWidget->rowCount(); i++) {
        const QVariant v = this->ui->tableWidget->item(i, 0)->
                data(Qt::UserRole);
        PackageVersion* f = (PackageVersion *) v.value<void*>();
        if (f == pv) {
            this->ui->tableWidget->selectRow(i);
            break;
        }
    }
}

PackageVersion* MainWindow::getSelectedPackageVersion()
{
    QList<QTableWidgetItem*> sel = this->ui->tableWidget->selectedItems();
    if (sel.count() > 0) {
        const QVariant v = sel.at(0)->data(Qt::UserRole);
        PackageVersion* pv = (PackageVersion *) v.value<void*>();
        return pv;
    }
    return 0;
}

void MainWindow::fillList()
{
    // qDebug() << "MainWindow::fillList";
    QTableWidget* t = this->ui->tableWidget;

    t->clearContents();
    t->setSortingEnabled(false);

    Repository* r = Repository::getDefault();

    t->setColumnCount(5);

    QTableWidgetItem *newItem = new QTableWidgetItem("Icon");
    t->setHorizontalHeaderItem(0, newItem);

    newItem = new QTableWidgetItem("Title");
    t->setHorizontalHeaderItem(1, newItem);

    newItem = new QTableWidgetItem("Description");
    t->setHorizontalHeaderItem(2, newItem);

    newItem = new QTableWidgetItem("Version");
    t->setHorizontalHeaderItem(3, newItem);

    newItem = new QTableWidgetItem("Status");
    t->setHorizontalHeaderItem(4, newItem);

    int statusFilter = this->ui->comboBoxStatus->currentIndex();
    QStringList textFilter =
            this->ui->lineEditText->text().toLower().simplified().split(" ");

    t->setRowCount(r->packageVersions.count());

    int n = 0;
    for (int i = 0; i < r->packageVersions.count(); i++) {
        PackageVersion* pv = r->packageVersions.at(i);

        bool installed = pv->installed();

        // filter by status (1, 2)
        if ((statusFilter == 1 && installed) ||
                (statusFilter == 2 && !installed))
            continue;

        // filter by text
        QString fullText = pv->getFullText();
        bool b = true;
        for (int i = 0; i < textFilter.count(); i++) {
            if (fullText.indexOf(textFilter.at(i)) < 0) {
                b = false;
                break;
            }
        }
        if (!b)
            continue;

        PackageVersion* newest = r->findNewestPackageVersion(pv->package);
        bool updateAvailable = newest->version.compare(pv->version) > 0;

        // filter by status (3)
        if (statusFilter == 3 && (!installed || !updateAvailable))
            continue;

        // filter by status (4)
        if (statusFilter == 4 && !(installed || !updateAvailable))
            continue;

        Package* p = r->findPackage(pv->package);

        newItem = new QTableWidgetItem("");
        newItem->setData(Qt::UserRole, qVariantFromValue((void*) pv));
        if (p) {
            if (!p->icon.isEmpty() && this->icons.contains(p->icon)) {
                QIcon icon = this->icons[p->icon];
                newItem->setIcon(icon);
            }
        }
        t->setItem(n, 0, newItem);

        QString packageTitle;
        if (p)
            packageTitle = p->title;
        else
            packageTitle = pv->package;
        newItem = new QTableWidgetItem(packageTitle);
        newItem->setStatusTip(pv->download.toString() + " " + pv->package +
                " " + pv->sha1);
        newItem->setData(Qt::UserRole, qVariantFromValue((void*) pv));
        t->setItem(n, 1, newItem);

        QString desc;
        if (p)
            desc = p->description;
        newItem = new QTableWidgetItem(desc);
        newItem->setData(Qt::UserRole, qVariantFromValue((void*) pv));
        t->setItem(n, 2, newItem);

        newItem = new QTableWidgetItem(pv->version.getVersionString());
        newItem->setData(Qt::UserRole, qVariantFromValue((void*) pv));
        t->setItem(n, 3, newItem);

        newItem = new QTableWidgetItem("");
        QString status;
        if (installed) {
            if (pv->external)
                status = "installed externally";
            else
                status = "installed";
        }
        if (installed && updateAvailable) {
            newItem->setBackgroundColor(QColor(255, 0xc7, 0xc7));
            status += ", update available";
        }
        newItem->setText(status);
        newItem->setData(Qt::UserRole, qVariantFromValue((void*) pv));
        t->setItem(n, 4, newItem);

        n++;
    }
    t->setRowCount(n);
    t->setSortingEnabled(true);
    // qDebug() << "MainWindow::fillList.2";
}

void MainWindow::process(QList<InstallOperation*> &install)
{
    // reoder the operations if a package is updated. In this case it is better
    // to uninstall the old first and then install the new one.
    if (install.size() == 2) {
        InstallOperation* first = install.at(0);
        InstallOperation* second = install.at(1);
        if (first->packageVersion->package == second->packageVersion->package &&
                first->install && !second->install) {
            install.insert(0, second);
            install.removeAt(2);
        }
    }

    PackageVersion* sel = getSelectedPackageVersion();

    QStringList locked = WPMUtils::getProcessFiles();
    QStringList lockedUninstall;
    for (int j = 0; j < install.size(); j++) {
        InstallOperation* op = install.at(j);
        if (!op->install) {
            PackageVersion* pv = op->packageVersion;
            QString path = pv->getDirectory().absolutePath();
            for (int i = 0; i < locked.size(); i++) {
                if (WPMUtils::isUnder(locked.at(i), path)) {
                    lockedUninstall.append(locked.at(i));
                }
            }
        }
    }

    if (lockedUninstall.size() > 0) {
        QString locked_ = lockedUninstall.join(", \n");
        QString msg("The packages cannot be uninstalled because "
                "the following files are in use "
                "(please close the corresponding applications): "
                "%1");
        QMessageBox::critical(this,
                "Uninstall", msg.arg(locked_));
        return;
    }

    for (int i = 0; i < install.count(); i++) {
        InstallOperation* op = install.at(i);
        if (!op->install) {
            PackageVersion* pv = op->packageVersion;
            if (pv->isDirectoryLocked()) {
                QDir d = pv->getDirectory();
                QString msg("The package %1 cannot be uninstalled because "
                        "some files or directories under %2 are in use.");
                QMessageBox::critical(this,
                        "Uninstall", msg.arg(pv->toString()).
                        arg(d.absolutePath()));
                return;
            }
        }
    }

    QString names;
    for (int i = 0; i < install.count(); i++) {
        InstallOperation* op = install.at(i);
        if (!op->install) {
            PackageVersion* pv = op->packageVersion;
            if (!names.isEmpty())
                names.append(", ");
            names.append(pv->toString());
            if (i > 5) {
                names.append("...");
                break;
            }
        }
    }
    QString installNames;
    for (int i = 0; i < install.count(); i++) {
        InstallOperation* op = install.at(i);
        if (op->install) {
            PackageVersion* pv = op->packageVersion;
            if (!installNames.isEmpty())
                installNames.append(", ");
            installNames.append(pv->toString());
            if (i > 5) {
                installNames.append("...");
                break;
            }
        }
    }

    int installCount = 0, uninstallCount = 0;
    for (int i = 0; i < install.count(); i++) {
        InstallOperation* op = install.at(i);
        if (op->install)
            installCount++;
        else
            uninstallCount++;
    }

    QMessageBox::StandardButton b;
    QString msg;
    if (installCount == 1 && uninstallCount == 0) {
        b = QMessageBox::Yes;
    } else if (installCount == 0 && uninstallCount == 1) {
        msg = QString("The package %1 will be uninstalled. "
                "The corresponding directory %2 "
                "will be completely deleted. "
                "There is no way to restore the files. "
                "Are you sure?").
                arg(install.at(0)->packageVersion->toString()).
                arg(install.at(0)->packageVersion->getDirectory().
                    absolutePath());
        b = QMessageBox::warning(this,
                "Uninstall", msg, QMessageBox::Yes | QMessageBox::No);
    } else if (installCount > 0 && uninstallCount == 0) {
        msg = QString("%1 package(s) will be installed: %2. "
                "Are you sure?").
                arg(installCount).arg(installNames);
        b = QMessageBox::warning(this,
                "Install", msg, QMessageBox::Yes | QMessageBox::No);
    } else if (installCount == 0 && uninstallCount > 0) {
        msg = QString("%1 package(s) will be uninstalled: %2. "
                "The corresponding directories "
                "will be completely deleted. "
                "There is no way to restore the files. "
                "Are you sure?").
                arg(uninstallCount).arg(names);
        b = QMessageBox::warning(this,
                "Uninstall", msg, QMessageBox::Yes | QMessageBox::No);
    } else {
        msg = QString("%1 package(s) will be uninstalled: "
            "%2 and %3 package(s) will be installed: %4. "
            "The corresponding directories "
            "will be completely deleted. "
            "There is no way to restore the files. "
            "Are you sure?").arg(uninstallCount).arg(names).
            arg(installCount).arg(installNames);
        b = QMessageBox::warning(this,
                "Uninstall", msg, QMessageBox::Yes | QMessageBox::No);
    }


    if (b == QMessageBox::Yes) {
        Job* job = new Job();
        InstallThread* it = new InstallThread(0, 1, job);
        it->install = install;
        it->start();
        it->setPriority(QThread::LowestPriority);

        waitFor(job, "Install/Uninstall");
        it->wait();
        delete it;
        delete job;

        fillList();
        selectPackageVersion(sel);
    }
}

void MainWindow::changeEvent(QEvent *e)
{
    QMainWindow::changeEvent(e);
    switch (e->type()) {
    case QEvent::LanguageChange:
        ui->retranslateUi(this);
        break;
    default:
        break;
    }
}

void MainWindow::on_actionExit_triggered()
{
    this->close();
}

void MainWindow::on_actionUninstall_activated()
{
    PackageVersion* pv = getSelectedPackageVersion();
    QList<InstallOperation*> ops;
    QList<PackageVersion*> installed = Repository::getDefault()->getInstalled();
    QString err = pv->planUninstallation(installed, ops);
    if (err.isEmpty())
        process(ops);
    else
        QMessageBox::critical(this,
                "Uninstall", err, QMessageBox::Ok);
}

void MainWindow::on_tableWidget_itemSelectionChanged()
{
    PackageVersion* pv = getSelectedPackageVersion();
    this->ui->actionInstall->setEnabled(pv && !pv->installed());
    this->ui->actionUninstall->setEnabled(pv && pv->installed() &&
            !pv->external);
    this->ui->actionCompute_SHA1->setEnabled(pv && pv->download.isValid());

    // "Update"
    if (pv && !pv->external) {
        Repository* r = Repository::getDefault();
        PackageVersion* newest = r->findNewestPackageVersion(pv->package);
        PackageVersion* newesti = r->findNewestInstalledPackageVersion(
                pv->package);
        this->ui->actionUpdate->setEnabled(
                newest != 0 && newesti != 0 &&
                newest->version.compare(newesti->version) > 0);
    } else {
        this->ui->actionUpdate->setEnabled(false);
    }

    // enable "Go To Package Page"
    Package* p;
    if (pv)
        p = Repository::getDefault()->findPackage(pv->package);
    else
        p = 0;
    this->ui->actionGotoPackageURL->setEnabled(pv && p &&
            QUrl(p->url).isValid());

    this->ui->actionTest_Download_Site->setEnabled(pv && p &&
            QUrl(p->url).isValid());
}

void MainWindow::recognizeAndloadRepositories()
{
    Job* job = new Job();
    InstallThread* it = new InstallThread(0, 3, job);
    it->start();
    it->setPriority(QThread::LowestPriority);

    QString title("Initializing");
    waitFor(job, title);
    it->wait();
    delete it;

    fillList();
    delete job;

    Repository* r = Repository::getDefault();
    for (int i = 0; i < r->packages.count(); i++) {
        Package* p = r->packages.at(i);
        if (!p->icon.isEmpty()) {
            FileLoaderItem it;
            it.url = p->icon;
            // qDebug() << "MainWindow::loadRepository " << it.url;
            this->fileLoader.work.append(it);
        }
    }
    // qDebug() << "MainWindow::loadRepository";
}

void MainWindow::on_actionInstall_activated()
{
    PackageVersion* pv = getSelectedPackageVersion();

    QList<PackageVersion*> r;
    QList<Dependency*> unsatisfiedDeps;
    pv->getInstallFirstPackages(r, unsatisfiedDeps);

    if (unsatisfiedDeps.count() > 0) {
        QString names;
        for (int i = 0; i < unsatisfiedDeps.count(); i++) {
            if (i != 0)
                names.append(", ");
            names.append(unsatisfiedDeps.at(i)->toString());
            if (i > 5) {
                names.append("...");
                break;
            }
        }
        QMessageBox::critical(this,
                "Error", QString("%1 dependencies cannot be satisfied. "
                "This package depends on %2, "
                "which are not available.").arg(unsatisfiedDeps.count()).
                arg(names),
                QMessageBox::Ok);
    } else {
        QList<InstallOperation*> ops;
        QList<PackageVersion*> installed =
                Repository::getDefault()->getInstalled();
        QString err = pv->planInstallation(installed, ops);
        if (err.isEmpty())
            process(ops);
        else
            QMessageBox::critical(this,
                    "Uninstall", err, QMessageBox::Ok);
    }
}

void MainWindow::on_actionGotoPackageURL_triggered()
{
    PackageVersion* pv = getSelectedPackageVersion();
    if (pv) {
        Package* p = Repository::getDefault()->findPackage(pv->package);
        if (p) {
            QUrl url(p->url);
            if (url.isValid()) {
                QDesktopServices::openUrl(url);
            }
        }
    }
}

void MainWindow::on_comboBoxStatus_currentIndexChanged(int index)
{
    this->fillList();
}

void MainWindow::on_lineEditText_textChanged(QString )
{
    this->fillList();
}

void MainWindow::on_actionSettings_triggered()
{
    SettingsDialog d;

    QList<QUrl*> urls = Repository::getRepositoryURLs();
    QStringList list;
    for (int i = 0; i < urls.count(); i++) {
        list.append(urls.at(i)->toString());
    }
    d.setRepositoryURLs(list);
    qDeleteAll(urls);
    urls.clear();

    d.setInstallationDirectory(WPMUtils::getInstallationDirectory());

    if (d.exec() == QDialog::Accepted) {
        list = d.getRepositoryURLs();
        if (list.count() == 0) {
            QMessageBox::critical(this,
                    "Error", "No repositories defined", QMessageBox::Ok);
        } else if (d.getInstallationDirectory().isEmpty()) {
            QMessageBox::critical(this,
                    "Error", "The installation directory cannot be empty",
                    QMessageBox::Ok);
        } else if (!QDir(d.getInstallationDirectory()).exists()) {
            QMessageBox::critical(this,
                    "Error", "The installation directory does not exist",
                    QMessageBox::Ok);
        } else {
            QString err;
            for (int i = 0; i < list.count(); i++) {
                QUrl* url = new QUrl(list.at(i));
                urls.append(url);
                if (!url->isValid()) {
                    err = QString("%1 is not a valid repository address").arg(
                            list.at(i));
                    break;
                }
            }
            if (err.isEmpty()) {
                WPMUtils::setInstallationDirectory(d.getInstallationDirectory());
                Repository::setRepositoryURLs(urls);
                recognizeAndloadRepositories();
            } else {
                QMessageBox::critical(this,
                        "Error", err, QMessageBox::Ok);
            }
            qDeleteAll(urls);
            urls.clear();
        }
    }
}

void MainWindow::on_actionUpdate_triggered()
{
    PackageVersion* pv = getSelectedPackageVersion();
    Repository* r = Repository::getDefault();
    PackageVersion* newest = r->findNewestPackageVersion(pv->package);
    PackageVersion* newesti = r->findNewestInstalledPackageVersion(pv->package);

    QList<InstallOperation*> ops;
    QList<PackageVersion*> installed =
            Repository::getDefault()->getInstalled();
    QString err = newest->planInstallation(installed, ops);
    if (err.isEmpty())
        err = newesti->planUninstallation(installed, ops);

    if (err.isEmpty()) {
        InstallOperation::simplify(ops);
        process(ops);
    } else
        QMessageBox::critical(this,
                "Uninstall", err, QMessageBox::Ok);
}

void MainWindow::on_actionTest_Download_Site_triggered()
{
    PackageVersion* pv = getSelectedPackageVersion();
    if (pv) {
        QString s = "http://www.urlvoid.com/scan/" + pv->download.host();
        QUrl url(s);
        if (url.isValid()) {
            QDesktopServices::openUrl(url);
        }
    }
}

void MainWindow::on_actionCompute_SHA1_triggered()
{
    PackageVersion* pv = getSelectedPackageVersion();
    Job* job = new Job();
    InstallThread* it = new InstallThread(pv, 5, job);
    it->start();
    it->setPriority(QThread::LowestPriority);

    waitFor(job, "Compute SHA1");
    it->wait();

    if (!job->isCancelled() && job->getErrorMessage().isEmpty()) {
        bool ok;
        QInputDialog::getText(this, "SHA1",
                QString("SHA1 for %1:").arg(pv->toString()), QLineEdit::Normal,
                it->sha1, &ok);
    }

    delete it;
    delete job;
}

void MainWindow::on_actionAbout_triggered()
{
    QUrl url("http://code.google.com/p/windows-package-manager");
    QDesktopServices::openUrl(url);
}
