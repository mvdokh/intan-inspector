#include "main_window.h"

#include <QComboBox>
#include <QCursor>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScreen>
#include <QSignalBlocker>
#include <QSlider>
#include <QSplitter>
#include <QTabWidget>
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

bool isPrimaryElectrodeLabel(const QString& label) {
    static const QRegularExpression pattern(QStringLiteral("^A-(\\d{3})$"));
    const QRegularExpressionMatch match = pattern.match(label.trimmed());
    if (!match.hasMatch()) {
        return false;
    }
    bool ok = false;
    const int numeric = match.captured(1).toInt(&ok);
    return ok && numeric >= 0 && numeric <= 31;
}

bool isAuxLikeLabel(const QString& label) {
    const QString upper = label.trimmed().toUpper();
    return upper.contains(QStringLiteral("AUX"))
        || upper.contains(QStringLiteral("DIGITAL IN"))
        || upper.startsWith(QStringLiteral("CH "))
        || upper.startsWith(QStringLiteral("CH-"))
        || upper == QStringLiteral("CH");
}

QRect initialWindowGeometry() {
    QScreen* screen = QGuiApplication::screenAt(QCursor::pos());
    if (screen == nullptr) {
        screen = QGuiApplication::primaryScreen();
    }
    if (screen == nullptr) {
        return QRect(80, 80, 1280, 820);
    }

    const QRect available = screen->availableGeometry();
    const int width = std::clamp(static_cast<int>(std::llround(available.width() * 0.86)), 980, available.width());
    const int height = std::clamp(static_cast<int>(std::llround(available.height() * 0.86)), 720, available.height());
    const int x = available.x() + ((available.width() - width) / 2);
    const int y = available.y() + ((available.height() - height) / 2);
    return QRect(x, y, width, height);
}

}  // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("Spike Sort Viewer"));
    setGeometry(initialWindowGeometry());
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
    configureChannelGroups();
    populateChannelLists();

    summaryLabel_->setText(
        QStringLiteral("%1 | %2 amp | %3 aux/digital | %4 Hz | %5 min")
            .arg(QFileInfo(recording_->infoPath).fileName())
            .arg(recording_->channelCount)
            .arg(std::max(0, recording_->channels.size() - recording_->channelCount))
            .arg(recording_->sampleRate, 0, 'f', 0)
            .arg(recording_->durationSeconds / 60.0, 0, 'f', 1)
    );

    const double maxTime = std::max(recording_->durationSeconds - windowSpin_->value(), 0.0);
    {
        const QSignalBlocker blocker(timeSpin_);
        timeSpin_->setRange(0.0, maxTime);
        timeSpin_->setValue(0.0);
    }
    {
        const QSignalBlocker blocker(timeSlider_);
        timeSlider_->setRange(0, std::max(0, static_cast<int>(std::llround(maxTime * 1000.0))));
        timeSlider_->setValue(0);
    }

    groupTabs_->setCurrentIndex(0);
    statusLabel_->setText(
        QStringLiteral("Loaded recording. Computing %1 overview for both channel tabs.")
            .arg(spikeviewer::transformModeName(currentTransformMode()))
    );

    refreshViews();
    for (int groupIndex = 0; groupIndex < groups_.size(); ++groupIndex) {
        startHeatmapWorker(groupIndex);
    }
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

    controlRow->addWidget(new QLabel(QStringLiteral("Stack Scale"), outer));
    scaleSpin_ = new QDoubleSpinBox(outer);
    scaleSpin_->setDecimals(2);
    scaleSpin_->setRange(0.1, 20.0);
    scaleSpin_->setSingleStep(0.1);
    scaleSpin_->setValue(1.0);
    controlRow->addWidget(scaleSpin_);
    connect(scaleSpin_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double) {
        refreshViews();
    });

    controlRow->addWidget(new QLabel(QStringLiteral("Detail Scale"), outer));
    detailScaleSpin_ = new QDoubleSpinBox(outer);
    detailScaleSpin_->setDecimals(2);
    detailScaleSpin_->setRange(0.05, 20.0);
    detailScaleSpin_->setSingleStep(0.05);
    detailScaleSpin_->setValue(0.5);
    controlRow->addWidget(detailScaleSpin_);
    connect(detailScaleSpin_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double) {
        refreshViews();
    });

    controlRow->addWidget(new QLabel(QStringLiteral("Transform"), outer));
    transformCombo_ = new QComboBox(outer);
    transformCombo_->addItems({
        QStringLiteral("Raw"),
        QStringLiteral("High-pass 300 Hz"),
        QStringLiteral("Band-pass 300-6000 Hz"),
        QStringLiteral("Band-pass 500-3000 Hz"),
        QStringLiteral("Low-pass 250 Hz"),
        QStringLiteral("Notch 60 Hz"),
    });
    controlRow->addWidget(transformCombo_);
    connect(transformCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        if (!recording_) {
            refreshViews();
            return;
        }
        statusLabel_->setText(
            QStringLiteral("Applying %1 and recomputing tab overviews.")
                .arg(spikeviewer::transformModeName(currentTransformMode()))
        );
        refreshViews();
        for (int groupIndex = 0; groupIndex < groups_.size(); ++groupIndex) {
            startHeatmapWorker(groupIndex);
        }
    });

    controlRow->addWidget(new QLabel(QStringLiteral("Overview"), outer));
    overviewCombo_ = new QComboBox(outer);
    overviewCombo_->addItems({
        QStringLiteral("Activity"),
        QStringLiteral("Events"),
        QStringLiteral("RMS"),
        QStringLiteral("Peak-to-Peak"),
        QStringLiteral("Population"),
        QStringLiteral("Motion"),
    });
    controlRow->addWidget(overviewCombo_);
    connect(overviewCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        refreshViews();
    });

    timeSlider_ = new QSlider(Qt::Horizontal, outer);
    timeSlider_->setRange(0, 1000);
    rootLayout->addWidget(timeSlider_);
    connect(timeSlider_, &QSlider::valueChanged, this, [this](int value) {
        if (!recording_) {
            return;
        }
        const double seconds = static_cast<double>(value) / 1000.0;
        {
            const QSignalBlocker blocker(timeSpin_);
            timeSpin_->setValue(seconds);
        }
        refreshViews();
    });

    statusLabel_ = new QLabel(QStringLiteral("Load an info.rhd to begin."), outer);
    statusLabel_->setWordWrap(true);
    rootLayout->addWidget(statusLabel_);

    groupTabs_ = new QTabWidget(outer);
    rootLayout->addWidget(groupTabs_, 1);
    connect(groupTabs_, &QTabWidget::currentChanged, this, [this](int) {
        refreshViews();
    });

    createGroupPage(QStringLiteral("Main Channels"));
    createGroupPage(QStringLiteral("Aux / Digital"));
}

void MainWindow::createGroupPage(const QString& baseTitle) {
    const int groupIndex = groups_.size();
    groups_.push_back(ChannelGroupUi{});
    ChannelGroupUi& group = groups_.last();
    group.baseTitle = baseTitle;

    auto* page = new QWidget(groupTabs_);
    auto* pageLayout = new QHBoxLayout(page);
    pageLayout->setContentsMargins(0, 0, 0, 0);
    pageLayout->setSpacing(4);

    auto* body = new QSplitter(Qt::Horizontal, page);
    body->setChildrenCollapsible(false);
    pageLayout->addWidget(body, 1);

    auto* sidebar = new QWidget(body);
    auto* sidebarLayout = new QVBoxLayout(sidebar);
    sidebarLayout->setContentsMargins(4, 4, 4, 4);
    sidebarLayout->setSpacing(6);
    sidebarLayout->addWidget(new QLabel(QStringLiteral("Channels"), sidebar));

    group.channelList = new QListWidget(sidebar);
    sidebarLayout->addWidget(group.channelList, 1);
    connect(group.channelList, &QListWidget::currentRowChanged, this, [this, groupIndex](int row) {
        if (groupIndex < 0 || groupIndex >= groups_.size()) {
            return;
        }
        groups_[groupIndex].selectedRow = row >= 0 ? row : -1;
        refreshViews();
    });

    auto* contentSplitter = new QSplitter(Qt::Vertical, body);
    contentSplitter->setChildrenCollapsible(false);
    group.traceView = new AllChannelsView(contentSplitter);
    group.traceView->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    auto* lowerSplitter = new QSplitter(Qt::Horizontal, contentSplitter);
    lowerSplitter->setChildrenCollapsible(false);
    auto* statsFrame = new QFrame(lowerSplitter);
    statsFrame->setFrameShape(QFrame::StyledPanel);
    statsFrame->setMinimumHeight(0);
    auto* statsLayout = new QVBoxLayout(statsFrame);
    statsLayout->setContentsMargins(8, 8, 8, 8);
    auto* statsScroll = new QScrollArea(statsFrame);
    statsScroll->setFrameShape(QFrame::NoFrame);
    statsScroll->setWidgetResizable(true);
    auto* statsScrollContent = new QWidget(statsScroll);
    auto* statsScrollLayout = new QVBoxLayout(statsScrollContent);
    statsScrollLayout->setContentsMargins(0, 0, 0, 0);
    statsScrollLayout->setSpacing(0);
    group.statsLabel = new QLabel(QStringLiteral("Select a channel"), statsScrollContent);
    group.statsLabel->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    group.statsLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    statsScrollLayout->addWidget(group.statsLabel, 0, Qt::AlignTop);
    statsScrollLayout->addStretch(1);
    statsScroll->setWidget(statsScrollContent);
    statsLayout->addWidget(statsScroll, 1);

    group.detailView = new DetailTraceView(lowerSplitter);
    group.detailView->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    group.overviewView = new OverviewView(lowerSplitter);
    group.overviewView->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    connect(group.overviewView, &OverviewView::timeJumpRequested, this, [this](double seconds) {
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

    body->setSizes({230, 1250});
    body->setStretchFactor(0, 0);
    body->setStretchFactor(1, 1);
    contentSplitter->setStretchFactor(0, 4);
    contentSplitter->setStretchFactor(1, 1);
    contentSplitter->setSizes({720, 260});
    lowerSplitter->setSizes({220, 520, 520});

    group.page = page;
    groupTabs_->addTab(page, baseTitle);
}

void MainWindow::configureChannelGroups() {
    if (!recording_) {
        for (ChannelGroupUi& group : groups_) {
            group.displayChannels.clear();
            group.selectedRow = -1;
            group.heatmap.reset();
            ++group.heatmapGeneration;
        }
        return;
    }

    const QVector<int> primaryChannels = detectPrimaryChannels();
    const QVector<int> auxChannels = detectAuxChannels(primaryChannels);

    if (!groups_.isEmpty()) {
        groups_[0].displayChannels = primaryChannels;
        groups_[0].selectedRow = -1;
        groups_[0].heatmap.reset();
        ++groups_[0].heatmapGeneration;
    }
    if (groups_.size() > 1) {
        groups_[1].displayChannels = auxChannels;
        groups_[1].selectedRow = -1;
        groups_[1].heatmap.reset();
        ++groups_[1].heatmapGeneration;
    }
}

QVector<int> MainWindow::detectPrimaryChannels() const {
    QVector<int> primary;
    if (!recording_) {
        return primary;
    }

    for (int channelIndex = 0; channelIndex < recording_->channels.size(); ++channelIndex) {
        const spikeviewer::ChannelInfo& channel = recording_->channels[channelIndex];
        if (!spikeviewer::isDigitalChannel(channel) && isPrimaryElectrodeLabel(channel.label)) {
            primary.push_back(channelIndex);
        }
    }
    if (!primary.isEmpty()) {
        return primary;
    }

    for (int channelIndex = 0; channelIndex < recording_->channels.size(); ++channelIndex) {
        const spikeviewer::ChannelInfo& channel = recording_->channels[channelIndex];
        if (!spikeviewer::isDigitalChannel(channel) && !isAuxLikeLabel(channel.label)) {
            primary.push_back(channelIndex);
        }
    }
    if (!primary.isEmpty()) {
        return primary;
    }

    for (int channelIndex = 0; channelIndex < recording_->channels.size(); ++channelIndex) {
        if (!spikeviewer::isDigitalChannel(recording_->channels[channelIndex])) {
            primary.push_back(channelIndex);
        }
    }
    if (!primary.isEmpty()) {
        return primary;
    }

    for (int channelIndex = 0; channelIndex < recording_->channels.size(); ++channelIndex) {
        primary.push_back(channelIndex);
    }
    return primary;
}

QVector<int> MainWindow::detectAuxChannels(const QVector<int>& primaryChannels) const {
    QVector<int> aux;
    if (!recording_) {
        return aux;
    }

    QVector<bool> isPrimary(recording_->channels.size(), false);
    for (int channelIndex : primaryChannels) {
        if (channelIndex >= 0 && channelIndex < isPrimary.size()) {
            isPrimary[channelIndex] = true;
        }
    }

    for (int channelIndex = 0; channelIndex < recording_->channels.size(); ++channelIndex) {
        if (!isPrimary[channelIndex]) {
            aux.push_back(channelIndex);
        }
    }
    return aux;
}

void MainWindow::populateChannelLists() {
    for (int groupIndex = 0; groupIndex < groups_.size(); ++groupIndex) {
        ChannelGroupUi& group = groups_[groupIndex];
        group.channelList->clear();
        if (recording_) {
            for (int displayChannel : group.displayChannels) {
                const spikeviewer::ChannelInfo& channel = recording_->channels[displayChannel];
                group.channelList->addItem(
                    QStringLiteral("%1 | %2 | (%3, %4)")
                        .arg(channel.electrodeId, 2, 10, QChar('0'))
                        .arg(channel.label)
                        .arg(channel.x, 0, 'g', 6)
                        .arg(channel.y, 0, 'g', 6)
                );
            }
        }

        if (group.selectedRow >= group.displayChannels.size()) {
            group.selectedRow = -1;
        }
        {
            const QSignalBlocker blocker(group.channelList);
            group.channelList->setCurrentRow(group.selectedRow);
        }
        groupTabs_->setTabText(groupIndex, QStringLiteral("%1 (%2)").arg(group.baseTitle).arg(group.displayChannels.size()));
    }
}

int MainWindow::activeGroupIndex() const {
    return groupTabs_ ? std::clamp(groupTabs_->currentIndex(), 0, std::max(groups_.size() - 1, 0)) : 0;
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

spikeviewer::TransformMode MainWindow::currentTransformMode() const {
    switch (transformCombo_->currentIndex()) {
    case 1:
        return spikeviewer::TransformMode::Highpass300;
    case 2:
        return spikeviewer::TransformMode::Bandpass300To6000;
    case 3:
        return spikeviewer::TransformMode::Bandpass500To3000;
    case 4:
        return spikeviewer::TransformMode::Lowpass250;
    case 5:
        return spikeviewer::TransformMode::Notch60;
    case 0:
    default:
        return spikeviewer::TransformMode::Raw;
    }
}

void MainWindow::updateSliderRange() {
    if (!recording_) {
        timeSlider_->setRange(0, 1000);
        return;
    }
    const double maxTime = std::max(recording_->durationSeconds - windowSpin_->value(), 0.0);
    const double clampedTime = std::clamp(timeSpin_->value(), 0.0, maxTime);
    {
        const QSignalBlocker blocker(timeSpin_);
        timeSpin_->setRange(0.0, maxTime);
        timeSpin_->setValue(clampedTime);
    }
    {
        const QSignalBlocker blocker(timeSlider_);
        timeSlider_->setRange(0, std::max(0, static_cast<int>(std::llround(maxTime * 1000.0))));
        timeSlider_->setValue(static_cast<int>(std::llround(clampedTime * 1000.0)));
    }
}

void MainWindow::refreshViews() {
    if (!recording_) {
        for (ChannelGroupUi& group : groups_) {
            group.traceView->setRecording({});
            group.detailView->setRecording({});
            group.overviewView->setRecording({});
            group.statsLabel->setText(QStringLiteral("Select a channel"));
        }
        statusLabel_->setText(QStringLiteral("Load an info.rhd to begin."));
        return;
    }

    updateSliderRange();
    const double maxTime = std::max(recording_->durationSeconds - windowSpin_->value(), 0.0);
    const double startTime = std::clamp(timeSpin_->value(), 0.0, maxTime);
    {
        const QSignalBlocker blocker(timeSpin_);
        timeSpin_->setValue(startTime);
    }
    {
        const QSignalBlocker blocker(timeSlider_);
        timeSlider_->setValue(static_cast<int>(std::llround(startTime * 1000.0)));
    }

    for (int groupIndex = 0; groupIndex < groups_.size(); ++groupIndex) {
        refreshGroupView(groupIndex);
    }

    const ChannelGroupUi& activeGroup = groups_[activeGroupIndex()];
    if (activeGroup.displayChannels.isEmpty()) {
        statusLabel_->setText(QStringLiteral("This tab has no channels for the current recording."));
    } else if (activeGroup.heatmap) {
        statusLabel_->setText(
            QStringLiteral("%1 ready with %2. Click the overview to jump through the recording.")
                .arg(activeGroup.baseTitle)
                .arg(spikeviewer::transformModeName(currentTransformMode()))
        );
    } else {
        statusLabel_->setText(
            QStringLiteral("Showing %1 from %2s to %3s with %4 while the overview recomputes.")
                .arg(activeGroup.baseTitle)
                .arg(startTime, 0, 'f', 4)
                .arg(startTime + windowSpin_->value(), 0, 'f', 4)
                .arg(spikeviewer::transformModeName(currentTransformMode()))
        );
    }
}

void MainWindow::refreshGroupView(int groupIndex) {
    if (groupIndex < 0 || groupIndex >= groups_.size()) {
        return;
    }

    ChannelGroupUi& group = groups_[groupIndex];
    group.traceView->setRecording(recording_);
    group.traceView->setDisplayChannels(group.displayChannels);
    group.traceView->setSelectedChannel(group.selectedRow);
    group.traceView->setTransformMode(currentTransformMode());
    group.traceView->setViewState(timeSpin_->value(), windowSpin_->value(), scaleSpin_->value());

    group.detailView->setRecording(recording_);
    group.detailView->setDisplayChannels(group.displayChannels);
    group.detailView->setSelectedChannel(group.selectedRow);
    group.detailView->setTransformMode(currentTransformMode());
    group.detailView->setViewState(timeSpin_->value(), windowSpin_->value(), detailScaleSpin_->value());

    group.overviewView->setRecording(recording_);
    group.overviewView->setHeatmap(group.heatmap);
    group.overviewView->setMode(currentOverviewMode());
    group.overviewView->setSelectedChannel(group.selectedRow);
    group.overviewView->setTimeSeconds(timeSpin_->value());

    updateGroupStatsPanel(groupIndex);
}

void MainWindow::updateGroupStatsPanel(int groupIndex) {
    if (groupIndex < 0 || groupIndex >= groups_.size()) {
        return;
    }

    ChannelGroupUi& group = groups_[groupIndex];
    if (!recording_ || group.selectedRow < 0 || group.selectedRow >= group.displayChannels.size()) {
        group.statsLabel->setStyleSheet({});
        group.statsLabel->setText(
            group.displayChannels.isEmpty()
                ? QStringLiteral("No channels in this tab")
                : QStringLiteral("Select a channel")
        );
        return;
    }

    const double startTime = timeSpin_->value();
    const auto [start, end] = timeWindowIndices(startTime);
    const int length = end - start;
    if (length <= 0) {
        group.statsLabel->setText(QStringLiteral("Select a channel"));
        return;
    }

    const int displayChannel = group.displayChannels[group.selectedRow];
    const spikeviewer::ChannelInfo& channel = recording_->channels[displayChannel];
    const QVector<float> signalVector = spikeviewer::extractChannelWindow(
        *recording_, channel, start, end, currentTransformMode());
    std::vector<float> signal(signalVector.begin(), signalVector.end());
    if (signal.empty()) {
        group.statsLabel->setText(QStringLiteral("Select a channel"));
        return;
    }

    const bool digitalChannel = spikeviewer::isDigitalChannel(channel);

    float minValue = std::numeric_limits<float>::infinity();
    float maxValue = -std::numeric_limits<float>::infinity();
    double sumSq = 0.0;
    double sumAbs = 0.0;
    for (float value : signal) {
        minValue = std::min(minValue, value);
        maxValue = std::max(maxValue, value);
        sumSq += value * value;
        sumAbs += std::abs(value);
    }

    const float rms = static_cast<float>(std::sqrt(sumSq / signal.size()));
    const float meanAbs = static_cast<float>(sumAbs / signal.size());
    const float ptp = maxValue - minValue;

    std::vector<float> windowPtps(static_cast<std::size_t>(group.displayChannels.size()));
    for (int row = 0; row < group.displayChannels.size(); ++row) {
        const spikeviewer::ChannelInfo& groupChannel = recording_->channels[group.displayChannels[row]];
        const QVector<float> groupSignal = spikeviewer::extractChannelWindow(
            *recording_, groupChannel, start, end, currentTransformMode());
        if (groupSignal.isEmpty()) {
            windowPtps[static_cast<std::size_t>(row)] = 0.0f;
            continue;
        }
        const auto minMax = std::minmax_element(groupSignal.begin(), groupSignal.end());
        windowPtps[static_cast<std::size_t>(row)] = *minMax.second - *minMax.first;
    }

    std::vector<int> rankOrder(group.displayChannels.size());
    std::iota(rankOrder.begin(), rankOrder.end(), 0);
    std::sort(rankOrder.begin(), rankOrder.end(), [&](int left, int right) {
        return windowPtps[static_cast<std::size_t>(left)] > windowPtps[static_cast<std::size_t>(right)];
    });
    const auto position = std::find(rankOrder.begin(), rankOrder.end(), group.selectedRow);
    const int rank = position != rankOrder.end() ? static_cast<int>(std::distance(rankOrder.begin(), position)) + 1 : group.displayChannels.size();

    QString thresholdText = QStringLiteral("n/a");
    QString eventCountText = QStringLiteral("n/a");
    if (digitalChannel) {
        int risingEdges = 0;
        float previous = signal.front();
        for (std::size_t index = 1; index < signal.size(); ++index) {
            if (signal[index] > 0.5f && previous <= 0.5f) {
                ++risingEdges;
            }
            previous = signal[index];
        }
        thresholdText = QStringLiteral("rising @ 0.5");
        eventCountText = QString::number(risingEdges);
    } else if (group.heatmap && group.selectedRow < group.heatmap->eventThresholds.size()) {
        const float median = medianCopy(signal);
        const float threshold = group.heatmap->eventThresholds[group.selectedRow];
        int crossings = 0;
        float previous = signal.front() - median;
        for (std::size_t index = 1; index < signal.size(); ++index) {
            const float centered = signal[index] - median;
            if (centered < threshold && previous >= threshold) {
                ++crossings;
            }
            previous = centered;
        }
        thresholdText = QStringLiteral("%1 uV").arg(threshold, 0, 'f', 1);
        eventCountText = QString::number(crossings);
    }

    const QString transformText = digitalChannel
        ? QStringLiteral("TTL raw (filters bypassed)")
        : spikeviewer::transformModeName(currentTransformMode());
    const QString units = digitalChannel ? QStringLiteral("TTL") : QStringLiteral("uV");
    const QString meanLabel = digitalChannel ? QStringLiteral("Mean High") : QStringLiteral("Mean |V|");

    const QColor color = channelColor(group.selectedRow, std::max(group.displayChannels.size(), 1));
    group.statsLabel->setStyleSheet(QStringLiteral("color: %1;").arg(color.name()));
    group.statsLabel->setText(
        QStringLiteral(
            "%1\n"
            "Electrode %2\n"
            "(%3, %4)\n"
            "\n"
            "Transform %5\n"
            "Window %6s\n"
            "to %7s\n"
            "\n"
            "Min %8 %9\n"
            "Max %10 %11\n"
            "P2P %12 %13\n"
            "RMS %14 %15\n"
            "%16 %17 %18\n"
            "Stack scale x%19\n"
            "Detail scale x%20\n"
            "Threshold %21\n"
            "Events %22\n"
            "\n"
            "Activity rank %23/%24"
        )
            .arg(channel.label)
            .arg(channel.electrodeId)
            .arg(channel.x, 0, 'g', 6)
            .arg(channel.y, 0, 'g', 6)
            .arg(transformText)
            .arg(startTime, 0, 'f', 4)
            .arg(startTime + windowSpin_->value(), 0, 'f', 4)
            .arg(minValue, 0, 'f', digitalChannel ? 3 : 1)
            .arg(units)
            .arg(maxValue, 0, 'f', digitalChannel ? 3 : 1)
            .arg(units)
            .arg(ptp, 0, 'f', digitalChannel ? 3 : 1)
            .arg(units)
            .arg(rms, 0, 'f', digitalChannel ? 3 : 1)
            .arg(units)
            .arg(meanLabel)
            .arg(meanAbs, 0, 'f', digitalChannel ? 3 : 1)
            .arg(units)
            .arg(scaleSpin_->value(), 0, 'f', 2)
            .arg(detailScaleSpin_->value(), 0, 'f', 2)
            .arg(thresholdText)
            .arg(eventCountText)
            .arg(rank)
            .arg(group.displayChannels.size())
    );
}

void MainWindow::startHeatmapWorker(int groupIndex) {
    if (!recording_ || groupIndex < 0 || groupIndex >= groups_.size()) {
        return;
    }

    ChannelGroupUi& group = groups_[groupIndex];
    group.heatmap.reset();
    const quint64 generation = ++group.heatmapGeneration;
    const std::shared_ptr<spikeviewer::RecordingData> recording = recording_;
    const QVector<int> displayChannels = group.displayChannels;
    const spikeviewer::TransformMode transformMode = currentTransformMode();

    if (displayChannels.isEmpty()) {
        refreshViews();
        return;
    }

    auto* watcher = new QFutureWatcher<spikeviewer::HeatmapResult>(this);
    connect(watcher, &QFutureWatcher<spikeviewer::HeatmapResult>::finished, this, [this, watcher, groupIndex, generation]() {
        const spikeviewer::HeatmapResult result = watcher->result();
        watcher->deleteLater();
        if (groupIndex < 0 || groupIndex >= groups_.size()) {
            return;
        }
        ChannelGroupUi& group = groups_[groupIndex];
        if (generation != group.heatmapGeneration) {
            return;
        }
        group.heatmap = std::make_shared<spikeviewer::HeatmapResult>(result);
        refreshViews();
    });

    watcher->setFuture(QtConcurrent::run([recording, displayChannels, transformMode]() {
        return spikeviewer::computeHeatmap(*recording, displayChannels, transformMode);
    }));
}
