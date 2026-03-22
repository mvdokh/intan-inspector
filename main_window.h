#pragma once

#include "recording.h"
#include "widgets.h"

#include <QMainWindow>
#include <QPointer>
#include <memory>

QT_BEGIN_NAMESPACE
class QComboBox;
class QDoubleSpinBox;
class QFutureWatcherBase;
template <typename T>
class QFutureWatcher;
class QLabel;
class QListWidget;
class QPushButton;
class QSlider;
QT_END_NAMESPACE

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    void loadPath(const QString& path);

private:
    void buildUi();
    void populateChannelList();
    void refreshViews();
    void updateSliderRange();
    void updateStatsPanel();
    void startHeatmapWorker();
    spikeviewer::OverviewMode currentOverviewMode() const;
    std::pair<int, int> timeWindowIndices(double startTime) const;

    std::shared_ptr<spikeviewer::RecordingData> recording_;
    std::shared_ptr<spikeviewer::HeatmapResult> heatmap_;
    quint64 heatmapGeneration_ = 0;
    int selectedChannel_ = -1;

    QPushButton* loadButton_ = nullptr;
    QLabel* summaryLabel_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QLabel* statsLabel_ = nullptr;
    QDoubleSpinBox* windowSpin_ = nullptr;
    QDoubleSpinBox* timeSpin_ = nullptr;
    QDoubleSpinBox* scaleSpin_ = nullptr;
    QSlider* timeSlider_ = nullptr;
    QListWidget* channelList_ = nullptr;
    QComboBox* overviewCombo_ = nullptr;
    AllChannelsView* traceView_ = nullptr;
    DetailTraceView* detailView_ = nullptr;
    OverviewView* overviewView_ = nullptr;
};
