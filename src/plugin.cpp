// Copyright (c) 2022-2025 Manuel Schneider

#include "plugin.h"
#include <QCheckBox>
#include <QCoroGenerator>
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
#include <albert/icon.h>
#include <albert/logging.h>
#include <albert/matcher.h>
#include <albert/plugin/snippets.h>
#include <albert/standarditem.h>
#include <albert/systemutil.h>
#include <albert/widgetsutil.h>
#include <mutex>
#include <shared_mutex>
ALBERT_LOGGING_CATEGORY("clipboard")
using namespace Qt::StringLiterals;
using namespace albert;
using namespace std;

namespace {
static const auto HISTORY_FILE_NAME  = u"clipboard_history"_s;
static const auto CFG_STORE_HISTORY  = u"persistent"_s;
static const auto DEF_STORE_HISTORY  = false;
static const auto CFG_HISTORY_LENGTH = u"history_length"_s;
static const auto DEF_HISTORY_LENGTH = 100u;
static const auto k_text             = u"text"_s;
static const auto k_datetime         = u"datetime"_s;
}


Plugin::Plugin():
    clipboard(QGuiApplication::clipboard())
{
    auto s = settings();
    store_history_ = s->value(CFG_STORE_HISTORY, DEF_STORE_HISTORY).toBool();
    history_limit_ = s->value(CFG_HISTORY_LENGTH, DEF_HISTORY_LENGTH).toUInt();

    if (store_history_)
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
    if (store_history_)
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

ItemGenerator Plugin::items(QueryContext &ctx)
{
    vector<shared_ptr<Item>> items;

    {
        QLocale loc;
        int rank = 0;
        Matcher matcher(ctx.query(), {.fuzzy=fuzzy});
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
                        [] { return Icon::grapheme(u"📋"_s); },
                        ::move(actions)
                    )
                );
            }
        }
    }

    co_yield items;
}

QWidget *Plugin::buildConfigWidget()
{
    auto *w = new QWidget;
    auto *l = new QFormLayout;

    auto *cb = new QCheckBox();
    cb->setChecked(store_history_);
    l->addRow(tr("Store history"), cb);
    bindWidget(cb, this, &Plugin::storeHistory, &Plugin::setStoreHistory);

    auto *s = new QSpinBox;
    s->setMinimum(1);
    s->setMaximum(10'000'000);
    s->setValue(history_limit_);
    l->addRow(tr("History limit"), s);
    bindWidget(s, this, &Plugin::historyLimit, &Plugin::setHistoryLimit);

    w->setLayout(l);
    return w;
}

uint Plugin::historyLimit() const { return history_limit_; }

void Plugin::setHistoryLimit(uint v)
{
    if (v != history_limit_)
    {
        history_limit_ = v;
        settings()->setValue(CFG_HISTORY_LENGTH, v);

        lock_guard lock(mutex);
        if (history_limit_ < history.size())
            history.resize(history_limit_);
    }
}

bool Plugin::storeHistory() const { return store_history_; }

void Plugin::setStoreHistory(bool v)
{
    if (v != store_history_)
    {
        store_history_ = v;
        settings()->setValue(CFG_STORE_HISTORY, v);
    }
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
    if (history_limit_ < history.size())
        history.resize(history_limit_);
}

bool Plugin::supportsFuzzyMatching() const { return true; }

void Plugin::setFuzzyMatching(bool enabled) { fuzzy = enabled; }
