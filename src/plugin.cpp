// Copyright (c) 2022-2025 Manuel Schneider

#include "plugin.h"
#include <QCheckBox>
#include <QDir>
#include <QFile>
#include <QFormLayout>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QSettings>
#include <QSpinBox>
#include <albert/extensionregistry.h>
#include <albert/iconutil.h>
#include <albert/logging.h>
#include <albert/matcher.h>
#include <albert/plugin/snippets.h>
#include <albert/standarditem.h>
#include <albert/systemutil.h>
#include <mutex>
#include <shared_mutex>
ALBERT_LOGGING_CATEGORY("clipboard")
using namespace Qt::StringLiterals;
using namespace albert::util;
using namespace albert;
using namespace std;

namespace {
static const auto HISTORY_FILE_NAME  = u"clipboard_history"_s;
static const auto CFG_PERSISTENCE    = u"persistent"_s;
static const auto DEF_PERSISTENCE    = false;
static const auto CFG_HISTORY_LENGTH = u"history_length"_s;
static const auto DEF_HISTORY_LENGTH = 100u;
static const auto k_text             = u"text"_s;
static const auto k_datetime         = u"datetime"_s;
}


Plugin::Plugin():
    clipboard(QGuiApplication::clipboard())
{
    // Load settings

    auto s = settings();
    persistent = s->value(CFG_PERSISTENCE, DEF_PERSISTENCE).toBool();
    length = s->value(CFG_HISTORY_LENGTH, DEF_HISTORY_LENGTH).toUInt();


    // Load history, if configured

    if (persistent)
    {
        if (QFile file(QDir(dataLocation()).filePath(HISTORY_FILE_NAME));
            file.open(QIODevice::ReadOnly | QIODevice::Text))
        {
            DEBG << "Reading clipboard history from" << file.fileName();
            const auto arr = QJsonDocument::fromJson(file.readAll()).array();
            for (const auto &value : arr)
            {
                const auto object = value.toObject();
                history.emplace_back(object[k_text].toString(),
                                     QDateTime::fromSecsSinceEpoch(object[k_datetime].toInt()));
            }
            file.close();
        }
        else
            DEBG << "Failed reading from clipboard history.";
    }

#if defined(Q_OS_MAC)
    // On macos dataChanged is not reliable. Poll
    connect(&timer, &QTimer::timeout, this, &Plugin::checkClipboard);
    timer.start(500);
#elif defined(Q_OS_UNIX)
    connect(clipboard, &QClipboard::changed, this,
            [this](QClipboard::Mode mode){ if (mode == QClipboard::Clipboard) checkClipboard(); });
#endif

}

Plugin::~Plugin()
{
    if (persistent)
    {
        QJsonArray array;
        for (const auto &entry : history)
        {
            QJsonObject object;
            object[k_text] = entry.text;
            object[k_datetime] = entry.datetime.toSecsSinceEpoch();
            array.append(object);
        }

        QDir data_dir = dataLocation();
        if (data_dir.exists() || data_dir.mkpath(u"."_s))
        {
            if (QFile file(data_dir.filePath(HISTORY_FILE_NAME));
                file.open(QIODevice::WriteOnly))
            {
                DEBG << "Writing clipboard history to" << file.fileName();
                file.write(QJsonDocument(array).toJson());
                file.close();
            }
            else
                WARN << "Failed creating history file:" << data_dir.path();
        }
        else
            WARN << "Failed creating data dir" << data_dir.path();
    }
}

void Plugin::handleTriggerQuery(Query &query)
{
    QLocale loc;
    int rank = 0;
    Matcher matcher(query.string(), {.fuzzy=fuzzy});
    vector<shared_ptr<Item>> items;

    shared_lock l(mutex);

    for (const auto &entry : history)
    {
        ++rank;
        if (matcher.match(entry.text))
        {
            static const auto tr_cp = tr("Copy and paste");
            static const auto tr_c = tr("Copy");
            static const auto tr_r = tr("Remove");

            vector<Action> actions;

            if(havePasteSupport())
                actions.emplace_back(
                    u"c"_s, tr_cp,
                    [t=entry.text](){ setClipboardTextAndPaste(t); }
                );

            actions.emplace_back(
                u"cp"_s, tr_c,
                [t=entry.text](){ setClipboardText(t); }
            );

            actions.emplace_back(
                u"r"_s, tr_r,
                [this, t=entry.text]()
                {
                    lock_guard lock(mutex);
                    this->history.remove_if([t](const auto& ce){ return ce.text == t; });
                }
            );

            if (snippets)
                actions.emplace_back(
                    u"s"_s, tr("Save as snippet"),
                    [this, t=entry.text]()
                    {
                        snippets->addSnippet(t);
                    });

            items.push_back(StandardItem::make(
                    id(),
                    entry.text,
                    u"#%1 %2"_s.arg(rank).arg(loc.toString(entry.datetime, QLocale::LongFormat)),
                    [] { return makeImageIcon(u":clipboard"_s); },
                    ::move(actions)
                )
            );
        }
    }

    query.add(items);
}

QWidget *Plugin::buildConfigWidget()
{
    auto *w = new QWidget;
    auto *l = new QFormLayout;

    auto *c = new QCheckBox();
    c->setChecked(persistent);
    c->setToolTip(tr("Stores the history on disk so that it persists across restarts."));
    l->addRow(tr("Store history"), c);
    connect(c, &QCheckBox::toggled, this, [this](bool checked)
            { settings()->setValue(CFG_PERSISTENCE, persistent = checked); });

    auto *s = new QSpinBox;
    s->setMinimum(1);
    s->setMaximum(10000000);
    s->setValue(length);
    l->addRow(tr("History length"), s);
    connect(s, &QSpinBox::valueChanged, this, [this](int value)
            {
                settings()->setValue(CFG_HISTORY_LENGTH, length = value);

                lock_guard lock(mutex);
                if (length < history.size())
                    history.resize(length);
            });

    w->setLayout(l);
    return w;
}

void Plugin::checkClipboard()
{
    // skip empty text (images, pixmaps etc), spaces only or no change
    if (auto text = clipboard->text();
        text.trimmed().isEmpty() || text == clipboard_text )
        return;
    else
        clipboard_text = text;

    lock_guard lock(mutex);

    // remove dups
    history.erase(remove_if(history.begin(), history.end(),
                            [this](const auto &ce) { return ce.text == clipboard_text; }),
                  history.end());

    // add an entry
    history.emplace_front(clipboard_text, QDateTime::currentDateTime());

    // adjust lenght
    if (length < history.size())
        history.resize(length);
}

bool Plugin::supportsFuzzyMatching() const { return true; }

void Plugin::setFuzzyMatching(bool enabled) { fuzzy = enabled; }
