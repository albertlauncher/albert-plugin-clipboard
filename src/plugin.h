// Copyright (c) 2022-2024 Manuel Schneider

#pragma once
#include <QClipboard>
#include <QDateTime>
#include <QTimer>
#include <albert/extensionplugin.h>
#include <albert/plugin/snippets.h>
#include <albert/plugindependency.h>
#include <albert/generatorqueryhandler.h>
#include <shared_mutex>


struct ClipboardEntry
{
    // required to allow list<ClipboardEntry>::resize
    // actually never used.
    ClipboardEntry() = default;
    ClipboardEntry(QString t, QDateTime dt) : text(std::move(t)), datetime(dt) {}
    QString text;
    QDateTime datetime;
};


class Plugin : public albert::ExtensionPlugin,
               public albert::GeneratorQueryHandler
{
    ALBERT_PLUGIN

public:

    Plugin();
    ~Plugin();

    bool supportsFuzzyMatching() const override;
    void setFuzzyMatching(bool enabled) override;
    albert::ItemGenerator items(albert::QueryContext &) override;
    QWidget *buildConfigWidget() override;

    uint historyLimit() const;
    void setHistoryLimit(uint);

    bool storeHistory() const;
    void setStoreHistory(bool);

    uint maxEntrySizeKiB() const;
    void setMaxEntrySizeKiB(uint);

private:
    void checkClipboard();

    QTimer timer;
    QClipboard * const clipboard;
    uint history_limit_;
    uint max_entry_bytes_;
    std::list<ClipboardEntry> history;
    bool store_history_;
    bool fuzzy;
    std::shared_mutex mutex;
    // explicit current, such that users can delete recent ones
    QString clipboard_text;
    
    albert::WeakDependency<snippets::Plugin> snippets{QStringLiteral("snippets")};
};
