#include "main_window.h"

#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFrame>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QSplitter>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

namespace {

QColor channelColor(int index, int total) {
    if (total <= 1) {
        return QColor::fromRgb(56, 189, 248);
    }
    const double hue = std::fmod(static_cast<double>(index) / std::max(total, 1), 1.0);
    return QColor::fromHsvF(hue, 0.82, 0.96);
}

float medianCopy(std::vector<float> values) {
    if (values.empty()) {
        return 0.0f;
    }
    const std::size_t mid = values.size() / 2;
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(mid), values.end());
    float upper = values[mid];
    if ((values.size() % 2U) == 1U) {
        return upper;
    }
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(mid - 1), values.begin() + static_cast<std::ptrdiff_t>(mid));
    return 0.5f * (upper + values[mid - 1]);
}

}  // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("Spike Sort Viewer"));
    resize(1460, 900);
    buildUi();
}

void MainWindow::loadPath(const QString& path) {
    QString error;
    const std::shared_ptr<spikeviewer::RecordingData> recording = spikeviewer::loadRecording(path, &error);
    if (!recording) {
        QMessageBox::critical(this, QStringLiteral("Load failed"), error);
        return;
    }

    recording_ = recording;
    heatmap_.reset();
    selectedChannel_ = -1;
    ++heatmapGeneration_;

    traceView_->setRecording(recording_);
    traceView_->setSelectedChannel(selectedChannel_);
    detailView_->setRecording(recording_);
    detailView_->setSelectedChannel(selectedChannel_);
    overviewView_->setRecording(recording_);
    overviewView_->setSelectedChannel(selectedChannel_);
    overviewView_->clearHeatmap();

    populateChannelList();

    const double maxTime = std::max(recording_->durationSeconds - windowSpin_->value(), 0.0);
    {
        const QSignalBlocker spinBlocker(timeSpin_);
        timeSpin_->setRange(0.0, maxTime);
        timeSpin_->setValue(0.0);
    }

    summaryLabel_->setText(
        QStringLiteral("%1 | %2 channels | %3 Hz | %4 min")
            .arg(QFileInfo(recording_->infoPath).fileName())
            .arg(recording_->channelCount)
            .arg(recording_->sampleRate, 0, 'f', 0)
            .arg(recording_->durationSeconds / 60.0, 0, 'f', 1)
    );
    statusLabel_->setText(QStringLiteral("Loaded recording. Computing activity heatmap in the background."));

    refreshViews();
    startHeatmapWorker();
}

void MainWindow::buildUi() {
    auto* outer = new QWidget(this);
    auto* rootLayout = new QVBoxLayout(outer);
    rootLayout->setContentsMargins(4, 4, 4, 4);
    rootLayout->setSpacing(4);
    setCentralWidget(outer);

    auto* controlRow = new QHBoxLayout();
    controlRow->setSpacing(8);
    rootLayout->addLayout(controlRow);

    loadButton_ = new QPushButton(QStringLiteral("Load info.rhd"), outer);
    controlRow->addWidget(loadButton_);
    connect(loadButton_, &QPushButton::clicked, this, [this]() {
        const QString path = QFileDialog::getOpenFileName(
            this,
            QStringLiteral("Select info.rhd"),
            QDir::currentPath(),
            QStringLiteral("Intan header (*.rhd);;All files (*.*)")
        );
        if (!path.isEmpty()) {
            loadPath(path);
        }
    });

    summaryLabel_ = new QLabel(outer);
    summaryLabel_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    controlRow->addWidget(summaryLabel_, 1);

    controlRow->addWidget(new QLabel(QStringLiteral("Window (s)"), outer));
    windowSpin_ = new QDoubleSpinBox(outer);
    windowSpin_->setDecimals(3);
    windowSpin_->setRange(0.001, 2.0);
    windowSpin_->setSingleStep(0.001);
    windowSpin_->setValue(0.02);
    controlRow->addWidget(windowSpin_);
    connect(windowSpin_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double) {
        refreshViews();
    });

    controlRow->addWidget(new QLabel(QStringLiteral("Time (s)"), outer));
    timeSpin_ = new QDoubleSpinBox(outer);
    timeSpin_->setDecimals(4);
    timeSpin_->setRange(0.0, 1.0);
    timeSpin_->setSingleStep(0.01);
    controlRow->addWidget(timeSpin_);
    connect(timeSpin_, &QDoubleSpinBox::editingFinished, this, [this]() {
        refreshViews();
    });

    auto* jumpButton = new QPushButton(QStringLiteral("Jump"), outer);
    controlRow->addWidget(jumpButton);
    connect(jumpButton, &QPushButton::clicked, this, [this]() {
        refreshViews();
    });

    controlRow->addWidget(new QLabel(QStringLiteral("Scale"), outer));
    scaleSpin_ = new QDoubleSpinBox(outer);
    scaleSpin_->setDecimals(2);
    scaleSpin_->setRange(0.1, 20.0);
    scaleSpin_->setSingleStep(0.1);
    scaleSpin_->setValue(1.0);
    controlRow->addWidget(scaleSpin_);
    connect(scaleSpin_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double) {
        refreshViews();
    });

    timeSlider_ = new QSlider(Qt::Horizontal, outer);
    timeSlider_->setRange(0, 1000);
    rootLayout->addWidget(timeSlider_);
    connect(timeSlider_, &QSlider::valueChanged, this, [this](int value) {
        if (!recording_) {
            return;
        }
        const double timeSeconds = static_cast<double>(value) / 1000.0;
        {
            const QSignalBlocker blocker(timeSpin_);
            timeSpin_->setValue(timeSeconds);
        }
        refreshViews();
    });

    auto* body = new QSplitter(Qt::Horizontal, outer);
    rootLayout->addWidget(body, 1);

    auto* sidebar = new QWidget(body);
    auto* sidebarLayout = new QVBoxLayout(sidebar);
    sidebarLayout->setContentsMargins(4, 4, 4, 4);
    sidebarLayout->setSpacing(6);
    sidebarLayout->addWidget(new QLabel(QStringLiteral("Channels"), sidebar));

    channelList_ = new QListWidget(sidebar);
    sidebarLayout->addWidget(channelList_, 1);
    connect(channelList_, &QListWidget::currentRowChanged, this, [this](int row) {
        selectedChannel_ = row >= 0 ? row : -1;
        refreshViews();
    });

    sidebarLayout->addWidget(new QLabel(QStringLiteral("Overview"), sidebar));
    overviewCombo_ = new QComboBox(sidebar);
    overviewCombo_->addItems({
        QStringLiteral("Activity"),
        QStringLiteral("Events"),
        QStringLiteral("RMS"),
        QStringLiteral("Peak-to-Peak"),
        QStringLiteral("Population"),
        QStringLiteral("Motion"),
    });
    sidebarLayout->addWidget(overviewCombo_);
    connect(overviewCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        refreshViews();
    });

    statusLabel_ = new QLabel(QStringLiteral("Load an info.rhd to begin."), sidebar);
    statusLabel_->setWordWrap(true);
    statusLabel_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    sidebarLayout->addWidget(statusLabel_);

    auto* contentSplitter = new QSplitter(Qt::Vertical, body);

    traceView_ = new AllChannelsView(contentSplitter);

    auto* lowerSplitter = new QSplitter(Qt::Horizontal, contentSplitter);
    auto* statsFrame = new QFrame(lowerSplitter);
    statsFrame->setFrameShape(QFrame::StyledPanel);
    auto* statsLayout = new QVBoxLayout(statsFrame);
    statsLayout->setContentsMargins(8, 8, 8, 8);
    statsLabel_ = new QLabel(QStringLiteral("Select a channel"), statsFrame);
    statsLabel_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    statsLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    statsLayout->addWidget(statsLabel_);

    detailView_ = new DetailTraceView(lowerSplitter);
    overviewView_ = new OverviewView(lowerSplitter);
    connect(overviewView_, &OverviewView::timeJumpRequested, this, [this](double seconds) {
        if (!recording_) {
            return;
        }
        const double maxTime = std::max(recording_->durationSeconds - windowSpin_->value(), 0.0);
        const double clamped = std::clamp(seconds, 0.0, maxTime);
        {
            const QSignalBlocker blocker(timeSpin_);
            timeSpin_->setValue(clamped);
        }
        refreshViews();
    });

    body->setSizes({220, 1240});
    contentSplitter->setStretchFactor(0, 4);
    contentSplitter->setStretchFactor(1, 1);
    lowerSplitter->setSizes({220, 520, 520});
}

void MainWindow::populateChannelList() {
    channelList_->clear();
    if (!recording_) {
        return;
    }
    for (const spikeviewer::ChannelInfo& channel : recording_->channels) {
        channelList_->addItem(
            QStringLiteral("%1 | %2 | (%3, %4)")
                .arg(channel.electrodeId, 2, 10, QChar('0'))
                .arg(channel.label)
                .arg(channel.x, 0, 'g', 6)
                .arg(channel.y, 0, 'g', 6)
        );
    }
    channelList_->clearSelection();
}

std::pair<int, int> MainWindow::timeWindowIndices(double startTime) const {
    if (!recording_) {
        return {0, 0};
    }
    const int start = std::clamp(static_cast<int>(std::floor(startTime * recording_->sampleRate)), 0, recording_->sampleCount);
    const int widthSamples = std::max(1, static_cast<int>(std::llround(windowSpin_->value() * recording_->sampleRate)));
    const int end = std::min(start + widthSamples, recording_->sampleCount);
    return {start, end};
}

spikeviewer::OverviewMode MainWindow::currentOverviewMode() const {
    switch (overviewCombo_->currentIndex()) {
    case 1:
        return spikeviewer::OverviewMode::Events;
    case 2:
        return spikeviewer::OverviewMode::RMS;
    case 3:
        return spikeviewer::OverviewMode::PeakToPeak;
    case 4:
        return spikeviewer::OverviewMode::Population;
    case 5:
        return spikeviewer::OverviewMode::Motion;
    case 0:
    default:
        return spikeviewer::OverviewMode::Activity;
    }
}

void MainWindow::updateSliderRange() {
    if (!recording_) {
        timeSlider_->setRange(0, 1000);
        return;
    }
    const double maxTime = std::max(recording_->durationSeconds - windowSpin_->value(), 0.0);
    const int maxSlider = std::max(0, static_cast<int>(std::llround(maxTime * 1000.0)));
    const double clampedTime = std::clamp(timeSpin_->value(), 0.0, maxTime);
    {
        const QSignalBlocker timeBlocker(timeSpin_);
        timeSpin_->setRange(0.0, maxTime);
        timeSpin_->setValue(clampedTime);
    }
    {
        const QSignalBlocker sliderBlocker(timeSlider_);
        timeSlider_->setRange(0, maxSlider);
        timeSlider_->setValue(static_cast<int>(std::llround(clampedTime * 1000.0)));
    }
}

void MainWindow::refreshViews() {
    if (!recording_) {
        traceView_->setRecording({});
        detailView_->setRecording({});
        overviewView_->setRecording({});
        statsLabel_->setText(QStringLiteral("Select a channel"));
        statusLabel_->setText(QStringLiteral("Load an info.rhd to begin."));
        return;
    }

    updateSliderRange();

    const double maxTime = std::max(recording_->durationSeconds - windowSpin_->value(), 0.0);
    const double startTime = std::clamp(timeSpin_->value(), 0.0, maxTime);
    {
        const QSignalBlocker timeBlocker(timeSpin_);
        timeSpin_->setValue(startTime);
    }
    {
        const QSignalBlocker sliderBlocker(timeSlider_);
        timeSlider_->setValue(static_cast<int>(std::llround(startTime * 1000.0)));
    }

    traceView_->setRecording(recording_);
    traceView_->setSelectedChannel(selectedChannel_);
    traceView_->setViewState(startTime, windowSpin_->value(), scaleSpin_->value());

    detailView_->setRecording(recording_);
    detailView_->setSelectedChannel(selectedChannel_);
    detailView_->setViewState(startTime, windowSpin_->value(), scaleSpin_->value());

    overviewView_->setRecording(recording_);
    overviewView_->setHeatmap(heatmap_);
    overviewView_->setMode(currentOverviewMode());
    overviewView_->setSelectedChannel(selectedChannel_);
    overviewView_->setTimeSeconds(startTime);

    updateStatsPanel();
    statusLabel_->setText(
        heatmap_
            ? QStringLiteral("Overview ready. Click it to jump through the recording.")
            : QStringLiteral("Showing all channels from %1s to %2s.")
                  .arg(startTime, 0, 'f', 4)
                  .arg(startTime + windowSpin_->value(), 0, 'f', 4)
    );
}

void MainWindow::updateStatsPanel() {
    if (!recording_ || selectedChannel_ < 0 || selectedChannel_ >= recording_->channels.size()) {
        statsLabel_->setStyleSheet({});
        statsLabel_->setText(QStringLiteral("Select a channel"));
        return;
    }

    const double startTime = timeSpin_->value();
    const auto [start, end] = timeWindowIndices(startTime);
    const int length = end - start;
    if (length <= 0) {
        statsLabel_->setText(QStringLiteral("Select a channel"));
        return;
    }

    const spikeviewer::ChannelInfo& channel = recording_->channels[selectedChannel_];
    std::vector<float> signal(static_cast<std::size_t>(length));
    float minValue = std::numeric_limits<float>::infinity();
    float maxValue = -std::numeric_limits<float>::infinity();
    double sumSq = 0.0;
    double sumAbs = 0.0;
    for (int offset = 0; offset < length; ++offset) {
        const float value = recording_->amplifier->sampleUv(start + offset, channel.index);
        signal[static_cast<std::size_t>(offset)] = value;
        minValue = std::min(minValue, value);
        maxValue = std::max(maxValue, value);
        sumSq += value * value;
        sumAbs += std::abs(value);
    }

    const float rms = static_cast<float>(std::sqrt(sumSq / length));
    const float meanAbs = static_cast<float>(sumAbs / length);
    const float ptp = maxValue - minValue;

    std::vector<float> windowPtps(static_cast<std::size_t>(recording_->channelCount));
    for (int physicalChannel = 0; physicalChannel < recording_->channelCount; ++physicalChannel) {
        float minChannel = std::numeric_limits<float>::infinity();
        float maxChannel = -std::numeric_limits<float>::infinity();
        for (int sampleIndex = start; sampleIndex < end; ++sampleIndex) {
            const float value = recording_->amplifier->sampleUv(sampleIndex, physicalChannel);
            minChannel = std::min(minChannel, value);
            maxChannel = std::max(maxChannel, value);
        }
        windowPtps[static_cast<std::size_t>(physicalChannel)] = maxChannel - minChannel;
    }
    std::vector<int> order(recording_->channelCount);
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](int left, int right) {
        return windowPtps[static_cast<std::size_t>(left)] > windowPtps[static_cast<std::size_t>(right)];
    });
    const auto position = std::find(order.begin(), order.end(), channel.index);
    const int rank = position != order.end() ? static_cast<int>(std::distance(order.begin(), position)) + 1 : recording_->channelCount;

    QString thresholdText = QStringLiteral("n/a");
    QString eventCountText = QStringLiteral("n/a");
    if (heatmap_ && channel.index < heatmap_->eventThresholds.size()) {
        const float median = medianCopy(signal);
        float previous = signal.front() - median;
        int crossings = 0;
        for (std::size_t i = 1; i < signal.size(); ++i) {
            const float centered = signal[i] - median;
            const float threshold = heatmap_->eventThresholds[channel.index];
            if (centered < threshold && previous >= threshold) {
                ++crossings;
            }
            previous = centered;
        }
        thresholdText = QStringLiteral("%1 uV").arg(heatmap_->eventThresholds[channel.index], 0, 'f', 1);
        eventCountText = QString::number(crossings);
    }

    const QColor color = channelColor(selectedChannel_, recording_->channelCount);
    statsLabel_->setStyleSheet(QStringLiteral("color: %1;").arg(color.name()));
    statsLabel_->setText(
        QStringLiteral(
            "%1\n"
            "Electrode %2\n"
            "(%3, %4)\n"
            "\n"
            "Window %5s\n"
            "to %6s\n"
            "\n"
            "Min %7 uV\n"
            "Max %8 uV\n"
            "P2P %9 uV\n"
            "RMS %10 uV\n"
            "Mean |V| %11 uV\n"
            "Threshold %12\n"
            "Events %13\n"
            "\n"
            "Activity rank %14/%15\n"
            "Display scale x%16"
        )
            .arg(channel.label)
            .arg(channel.electrodeId)
            .arg(channel.x, 0, 'g', 6)
            .arg(channel.y, 0, 'g', 6)
            .arg(startTime, 0, 'f', 4)
            .arg(startTime + windowSpin_->value(), 0, 'f', 4)
            .arg(minValue, 0, 'f', 1)
            .arg(maxValue, 0, 'f', 1)
            .arg(ptp, 0, 'f', 1)
            .arg(rms, 0, 'f', 1)
            .arg(meanAbs, 0, 'f', 1)
            .arg(thresholdText)
            .arg(eventCountText)
            .arg(rank)
            .arg(recording_->channelCount)
            .arg(scaleSpin_->value(), 0, 'f', 2)
    );
}

void MainWindow::startHeatmapWorker() {
    if (!recording_) {
        return;
    }

    const quint64 generation = heatmapGeneration_;
    auto* watcher = new QFutureWatcher<spikeviewer::HeatmapResult>(this);
    connect(watcher, &QFutureWatcher<spikeviewer::HeatmapResult>::finished, this, [this, watcher, generation]() {
        const spikeviewer::HeatmapResult result = watcher->result();
        watcher->deleteLater();
        if (generation != heatmapGeneration_) {
            return;
        }
        heatmap_ = std::make_shared<spikeviewer::HeatmapResult>(result);
        overviewView_->setHeatmap(heatmap_);
        refreshViews();
    });

    const std::shared_ptr<spikeviewer::RecordingData> recording = recording_;
    watcher->setFuture(QtConcurrent::run([recording]() {
        return spikeviewer::computeHeatmap(*recording);
    }));
}
