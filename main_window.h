#pragma once

#include "recording.h"
#include "widgets.h"

#include <QMainWindow>
#include <memory>

QT_BEGIN_NAMESPACE
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QListWidget;
class QPushButton;
class QSlider;
class QTabWidget;
class QWidget;
QT_END_NAMESPACE

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    void loadPath(const QString& path);

private:
    struct ChannelGroupUi {
        QString baseTitle;
        QVector<int> displayChannels;
        int selectedRow = -1;
        std::shared_ptr<spikeviewer::HeatmapResult> heatmap;
        quint64 heatmapGeneration = 0;
        QWidget* page = nullptr;
        QListWidget* channelList = nullptr;
        QLabel* statsLabel = nullptr;
        AllChannelsView* traceView = nullptr;
        DetailTraceView* detailView = nullptr;
        OverviewView* overviewView = nullptr;
    };

    void buildUi();
    void createGroupPage(const QString& baseTitle);
    void configureChannelGroups();
    void populateChannelLists();
    void refreshViews();
    void refreshGroupView(int groupIndex);
    void updateSliderRange();
    void updateGroupStatsPanel(int groupIndex);
    void startHeatmapWorker(int groupIndex);
    int activeGroupIndex() const;
    spikeviewer::OverviewMode currentOverviewMode() const;
    spikeviewer::TransformMode currentTransformMode() const;
    std::pair<int, int> timeWindowIndices(double startTime) const;
    QVector<int> detectPrimaryChannels() const;
    QVector<int> detectAuxChannels(const QVector<int>& primaryChannels) const;

    std::shared_ptr<spikeviewer::RecordingData> recording_;
    QVector<ChannelGroupUi> groups_;

    QPushButton* loadButton_ = nullptr;
    QLabel* summaryLabel_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QDoubleSpinBox* windowSpin_ = nullptr;
    QDoubleSpinBox* timeSpin_ = nullptr;
    QDoubleSpinBox* scaleSpin_ = nullptr;
    QDoubleSpinBox* detailScaleSpin_ = nullptr;
    QSlider* timeSlider_ = nullptr;
    QComboBox* overviewCombo_ = nullptr;
    QComboBox* transformCombo_ = nullptr;
    QTabWidget* groupTabs_ = nullptr;
};
