// Copyright (c) 2017 Manuel Schneider

#include <pybind11/embed.h>
#include <pybind11/stl.h>
#include "pythonmodulev1.h"
#include <QProcess>
#include <QFileSystemWatcher>
#include <QMutex>
#include <QByteArray>
#include <QFileInfo>
#include <QDebug>
#include <QDirIterator>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QProcess>
#include <QRegularExpression>
#include <QVBoxLayout>
#include <functional>
#include <vector>
#include "core/query.h"
#include "util/standarditem.h"
#include "xdg/iconlookup.h"
#include "cast_specialization.h"
using namespace std;
using namespace Core;
namespace py = pybind11;

namespace {
uint majorInterfaceVersion = 0;
uint minorInterfaceVersion = 2;
}

class Python::PythonModuleV1Private
{
public:
    QString path;
    QString name;
    QString id;  // Effectively the module name
    PythonModuleV1::State state;
    QString errorString;
    QString author;
    QString version;
    QString trigger;
    QString description;
    QStringList dependencies;
    QFileSystemWatcher fileSystemWatcher;
    py::module module;
};

/** ***************************************************************************/
Python::PythonModuleV1::PythonModuleV1(const QString &path) : d(new PythonModuleV1Private) {
    d->path = path;
    d->id = d->name = QFileInfo(d->path).completeBaseName();
    d->state = State::Unloaded;

    connect(&d->fileSystemWatcher, &QFileSystemWatcher::fileChanged,
            this, &PythonModuleV1::unload);
    connect(&d->fileSystemWatcher, &QFileSystemWatcher::fileChanged,
            this, &PythonModuleV1::load);

    connect(&d->fileSystemWatcher, &QFileSystemWatcher::directoryChanged,
            this, &PythonModuleV1::unload);
    connect(&d->fileSystemWatcher, &QFileSystemWatcher::directoryChanged,
            this, &PythonModuleV1::load);

}


/** ***************************************************************************/
Python::PythonModuleV1::~PythonModuleV1() {
    unload();
}


/** ***************************************************************************/
void Python::PythonModuleV1::load(){

    if (d->state == State::Loaded)
        return;

    py::gil_scoped_acquire acquire;

    QFileInfo fileInfo(d->path);
    try
    {
        d->module = py::module::import(fileInfo.completeBaseName().toUtf8().data());
        d->module.reload();  // Drop cached module version

        qDebug() << "Loading" << d->path;

        QString iid = d->module.attr("__iid__").cast<QString>();
        QRegularExpression re("^PythonInterface\\/v(\\d)\\.(\\d)$");
        QRegularExpressionMatch match = re.match(iid);
        if (!match.hasMatch()) {
            d->errorString = "Incompatible interface id";
            d->state = State::Error;
            qWarning() << qPrintable(QString("[%1] %2.").arg(QFileInfo(d->path).fileName()).arg(d->errorString));
            return;
        }

        uint maj = match.captured(1).toUInt();
        if (maj != majorInterfaceVersion) {
            d->errorString = QString("Incompatible major interface version. Expected %1, got %2").arg(majorInterfaceVersion).arg(maj);
            d->state = State::Error;
            qWarning() << qPrintable(QString("[%1] %2.").arg(QFileInfo(d->path).fileName()).arg(d->errorString));
            return;
        }

        uint min = match.captured(2).toUInt();
        if (min > minorInterfaceVersion) {
            d->errorString = QString("Incompatible minor interface version. Up to %1 supported, got %2").arg(minorInterfaceVersion).arg(min);
            d->state = State::Error;
            qWarning() << qPrintable(QString("[%1] %2.").arg(QFileInfo(d->path).fileName()).arg(d->errorString));
            return;
        }

        if (py::hasattr(d->module, "__prettyname__"))
            d->name = d->module.attr("__prettyname__").cast<QString>();

        if (py::hasattr(d->module.ptr(), "__version__"))
            d->version = d->module.attr("__version__").cast<QString>();

        if (py::hasattr(d->module.ptr(), "__author__"))
            d->author = d->module.attr("__author__").cast<QString>();

        if (py::hasattr(d->module.ptr(), "__doc__")){
            py::object docString = d->module.attr("__doc__");
            if (!docString.is_none())
                d->description = docString.cast<QString>();
        }

        if (py::hasattr(d->module.ptr(), "__trigger__")){
            py::object trigger = d->module.attr("__trigger__");
            if (!trigger.is_none())
                d->trigger = trigger.cast<QString>();
        }

        if (py::hasattr(d->module.ptr(), "__dependencies__")){
            py::list deps = d->module.attr("__dependencies__").cast<py::list>();
            d->dependencies.clear();
            for(py::size_t i = 0; i < py::len(deps); i++)
                d->dependencies.append(deps[i].cast<QString>());
        }

        if (py::hasattr(d->module, "initialize")) {
            py::object init = d->module.attr("initialize");
            if (py::isinstance<py::function>(init))
                init();
        }
    }
    catch(std::exception const &e)
    {
        d->errorString = e.what();
        qWarning() << qPrintable(QString("[%1] %2.").arg(QFileInfo(d->path).fileName()).arg(d->errorString));
        d->module = py::object();
        d->state = State::Error;
        return;
    }

    if (fileInfo.isDir()) {
        QDirIterator dit(d->path, QDir::Dirs|QDir::NoDotDot, QDirIterator::Subdirectories);
        while (dit.hasNext())
            if (dit.next() != "__pycache__")
                d->fileSystemWatcher.addPath(dit.path());
        QDirIterator fit(d->path, {"*.py"}, QDir::Files, QDirIterator::Subdirectories);
        while (fit.hasNext())
            d->fileSystemWatcher.addPath(fit.next());
    } else
        d->fileSystemWatcher.addPath(d->path);

    d->state = State::Loaded;
    emit moduleChanged();
}


/** ***************************************************************************/
void Python::PythonModuleV1::unload(){

    if (d->state == State::Unloaded)
        return;

    if (d->state == State::Loaded) {

        qDebug() << "Unloading" << d->path;

        py::gil_scoped_acquire acquire;

        try
        {
            if (py::hasattr(d->module, "finalize")) {
                py::object fini = d->module.attr("finalize");
                if (py::isinstance<py::function>(fini))
                    fini();
            }
            d->module = py::object();
        }
        catch(std::exception const &e)
        {
            qWarning() << qPrintable(QString("[%1] %2.").arg(QFileInfo(d->path).fileName()).arg(e.what()));
        }

        if (!d->fileSystemWatcher.files().isEmpty())
            d->fileSystemWatcher.removePaths(d->fileSystemWatcher.files());
        if (!d->fileSystemWatcher.files().isEmpty())
            d->fileSystemWatcher.removePaths(d->fileSystemWatcher.directories());
    }

    d->errorString.clear();
    d->state = State::Unloaded;
    emit moduleChanged();
}


/** ***************************************************************************/
void Python::PythonModuleV1::handleQuery(Query *query) const {

    py::gil_scoped_acquire acquire;

    try {
        vector<pair<shared_ptr<Core::Item>,uint>> results;
        py::function f = py::function(d->module.attr("handleQuery"));
        py::object pythonResult = f(query);

        if ( !query->isValid() )
            return;

        if (py::isinstance<py::list>(pythonResult)) {

            py::list list(pythonResult);
            for(py::size_t i = 0; i < py::len(pythonResult); ++i) {
                py::object elem = list[i];
                results.emplace_back(elem.cast<shared_ptr<StandardItem>>(), 0);
            }

            query->addMatches(std::make_move_iterator(results.begin()),
                              std::make_move_iterator(results.end()));
        }

        if (py::isinstance<Item>(pythonResult)) {
            query->addMatch(pythonResult.cast<shared_ptr<StandardItem>>());
        }
    }
    catch(const exception &e)
    {
        qWarning() << qPrintable(QString("[%1] %2.").arg(d->id).arg(e.what()));
    }
}


/** ***************************************************************************/
Python::PythonModuleV1::State Python::PythonModuleV1::state() const { return d->state; }
const QString &Python::PythonModuleV1::errorString() const { return d->errorString; }
const QString &Python::PythonModuleV1::path() const { return d->path; }
const QString &Python::PythonModuleV1::id() const { return d->id; }
const QString &Python::PythonModuleV1::name() const { return d->name; }
const QString &Python::PythonModuleV1::author() const { return d->author; }
const QString &Python::PythonModuleV1::version() const { return d->version; }
const QString &Python::PythonModuleV1::description() const { return d->description; }
const QString &Python::PythonModuleV1::trigger() const { return d->trigger; }
const QStringList &Python::PythonModuleV1::dependencies() const { return d->dependencies; }



