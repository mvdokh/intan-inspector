#include "widgets.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace {

constexpr QRgb kPlotBackground = 0xff0b1020;
constexpr QRgb kPlotGrid = 0x40e5e7eb;
constexpr QRgb kCursor = 0xfff8fafc;
constexpr QRgb kEmptyText = 0xff94a3b8;
constexpr QRgb kHighlight = 0xff22d3ee;

QColor channelColor(int index, int total) {
    if (total <= 1) {
        return QColor::fromRgb(56, 189, 248);
    }
    const double hue = std::fmod(static_cast<double>(index) / std::max(total, 1), 1.0);
    return QColor::fromHsvF(hue, 0.82, 0.96);
}

float percentileCopy(std::vector<float> values, float percentile) {
    if (values.empty()) {
        return 0.0f;
    }
    percentile = std::clamp(percentile, 0.0f, 1.0f);
    const std::size_t index = static_cast<std::size_t>(std::floor(percentile * static_cast<float>(values.size() - 1)));
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(index), values.end());
    return values[index];
}

struct DownsampledTrace {
    QVector<float> x;
    QVector<float> y;
};

DownsampledTrace downsampleTrace(const std::vector<float>& signal, int targetPoints) {
    DownsampledTrace result;
    if (signal.empty()) {
        return result;
    }

    targetPoints = std::max(targetPoints, 2);
    if (static_cast<int>(signal.size()) <= targetPoints) {
        result.x.resize(static_cast<int>(signal.size()));
        result.y.resize(static_cast<int>(signal.size()));
        for (int i = 0; i < static_cast<int>(signal.size()); ++i) {
            result.x[i] = static_cast<float>(i) / static_cast<float>(signal.size());
            result.y[i] = signal[static_cast<std::size_t>(i)];
        }
        return result;
    }

    const int step = static_cast<int>(std::ceil(static_cast<double>(signal.size()) / targetPoints));
    const int trimmed = static_cast<int>(signal.size()) - (static_cast<int>(signal.size()) % step);
    if (trimmed <= 0) {
        result.x.resize(static_cast<int>(signal.size()));
        result.y.resize(static_cast<int>(signal.size()));
        for (int i = 0; i < static_cast<int>(signal.size()); ++i) {
            result.x[i] = static_cast<float>(i) / static_cast<float>(signal.size());
            result.y[i] = signal[static_cast<std::size_t>(i)];
        }
        return result;
    }

    const int groups = trimmed / step;
    result.x.resize(groups * 2);
    result.y.resize(groups * 2);
    for (int group = 0; group < groups; ++group) {
        float minValue = std::numeric_limits<float>::infinity();
        float maxValue = -std::numeric_limits<float>::infinity();
        const int base = group * step;
        for (int offset = 0; offset < step; ++offset) {
            const float value = signal[static_cast<std::size_t>(base + offset)];
            minValue = std::min(minValue, value);
            maxValue = std::max(maxValue, value);
        }
        result.x[group * 2] = static_cast<float>(group) / std::max(groups, 1);
        result.x[(group * 2) + 1] = static_cast<float>(group) / std::max(groups, 1);
        result.y[group * 2] = minValue;
        result.y[(group * 2) + 1] = maxValue;
    }
    return result;
}

QRect insetRect(const QRect& rect, int left, int top, int right, int bottom) {
    return rect.adjusted(left, top, -right, -bottom);
}

float normalizeToRange(float value, float minValue, float maxValue) {
    if (maxValue <= minValue) {
        return 0.5f;
    }
    return (value - minValue) / (maxValue - minValue);
}

QColor lerpColor(const QColor& a, const QColor& b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return QColor::fromRgbF(
        a.redF() + ((b.redF() - a.redF()) * t),
        a.greenF() + ((b.greenF() - a.greenF()) * t),
        a.blueF() + ((b.blueF() - a.blueF()) * t),
        1.0
    );
}

QColor gradientColor(const QVector<QPair<float, QColor>>& stops, float value) {
    if (stops.isEmpty()) {
        return QColor::fromRgb(255, 255, 255);
    }
    value = std::clamp(value, 0.0f, 1.0f);
    for (int i = 1; i < stops.size(); ++i) {
        if (value <= stops[i].first) {
            const float span = std::max(stops[i].first - stops[i - 1].first, 1.0e-6f);
            const float t = (value - stops[i - 1].first) / span;
            return lerpColor(stops[i - 1].second, stops[i].second, t);
        }
    }
    return stops.back().second;
}

QVector<QPair<float, QColor>> paletteForMode(spikeviewer::OverviewMode mode) {
    using Stop = QPair<float, QColor>;
    switch (mode) {
    case spikeviewer::OverviewMode::Activity:
        return {
            Stop(0.0f, QColor(6, 4, 25)),
            Stop(0.35f, QColor(75, 23, 107)),
            Stop(0.7f, QColor(188, 55, 84)),
            Stop(1.0f, QColor(251, 252, 191)),
        };
    case spikeviewer::OverviewMode::Events:
        return {
            Stop(0.0f, QColor(0, 0, 4)),
            Stop(0.25f, QColor(66, 10, 104)),
            Stop(0.65f, QColor(203, 70, 31)),
            Stop(1.0f, QColor(252, 255, 164)),
        };
    case spikeviewer::OverviewMode::RMS:
        return {
            Stop(0.0f, QColor(68, 1, 84)),
            Stop(0.35f, QColor(49, 104, 142)),
            Stop(0.7f, QColor(53, 183, 121)),
            Stop(1.0f, QColor(253, 231, 37)),
        };
    case spikeviewer::OverviewMode::PeakToPeak:
        return {
            Stop(0.0f, QColor(13, 8, 135)),
            Stop(0.32f, QColor(126, 3, 168)),
            Stop(0.68f, QColor(225, 100, 98)),
            Stop(1.0f, QColor(240, 249, 33)),
        };
    case spikeviewer::OverviewMode::Population:
    case spikeviewer::OverviewMode::Motion:
        break;
    }
    return {};
}

QString colorbarLabel(spikeviewer::OverviewMode mode) {
    switch (mode) {
    case spikeviewer::OverviewMode::Activity:
        return QStringLiteral("activity");
    case spikeviewer::OverviewMode::Events:
        return QStringLiteral("events/bin");
    case spikeviewer::OverviewMode::RMS:
        return QStringLiteral("uV");
    case spikeviewer::OverviewMode::PeakToPeak:
        return QStringLiteral("uV");
    case spikeviewer::OverviewMode::Population:
        return QStringLiteral("population");
    case spikeviewer::OverviewMode::Motion:
        return QStringLiteral("motion");
    }
    return {};
}

const QVector<float>* matrixForMode(const std::shared_ptr<spikeviewer::HeatmapResult>& heatmap, spikeviewer::OverviewMode mode) {
    if (!heatmap) {
        return nullptr;
    }
    switch (mode) {
    case spikeviewer::OverviewMode::Activity:
        return &heatmap->activityMatrix;
    case spikeviewer::OverviewMode::Events:
        return &heatmap->eventMatrix;
    case spikeviewer::OverviewMode::RMS:
        return &heatmap->rmsMatrix;
    case spikeviewer::OverviewMode::PeakToPeak:
        return &heatmap->ptpMatrix;
    case spikeviewer::OverviewMode::Population:
    case spikeviewer::OverviewMode::Motion:
        return nullptr;
    }
    return nullptr;
}

QVector<float> normalizeSeries(const QVector<float>& series) {
    QVector<float> output(series.size());
    if (series.isEmpty()) {
        return output;
    }

    std::vector<float> values(series.begin(), series.end());
    const float median = percentileCopy(values, 0.5f);

    values.clear();
    values.reserve(static_cast<std::size_t>(series.size()));
    for (float value : series) {
        values.push_back(std::abs(value - median));
    }
    const float scale = std::max(percentileCopy(values, 0.95f), 1.0e-6f);

    for (int i = 0; i < series.size(); ++i) {
        output[i] = (series[i] - median) / scale;
    }
    return output;
}

void drawPolylineSeries(QPainter& painter, const QRect& rect, const QVector<float>& times, const QVector<float>& series, const QColor& color) {
    if (times.size() < 2 || times.size() != series.size()) {
        return;
    }

    const float minTime = times.first();
    const float maxTime = times.last();
    if (maxTime <= minTime) {
        return;
    }

    QPolygonF line;
    line.reserve(times.size());
    for (int i = 0; i < times.size(); ++i) {
        const float xRatio = normalizeToRange(times[i], minTime, maxTime);
        const float yRatio = normalizeToRange(series[i], -1.25f, 1.25f);
        const qreal x = rect.left() + (xRatio * rect.width());
        const qreal y = rect.bottom() - (yRatio * rect.height());
        line.append(QPointF(x, y));
    }

    QPen pen(color);
    pen.setWidthF(1.3);
    pen.setCosmetic(true);
    painter.setPen(pen);
    painter.drawPolyline(line);
}

}  // namespace

AllChannelsView::AllChannelsView(QWidget* parent)
    : QWidget(parent) {
    setMinimumHeight(220);
}

void AllChannelsView::setRecording(const std::shared_ptr<spikeviewer::RecordingData>& recording) {
    recording_ = recording;
    update();
}

void AllChannelsView::setDisplayChannels(const QVector<int>& displayChannels) {
    displayChannels_ = displayChannels;
    update();
}

void AllChannelsView::setSelectedChannel(int selectedChannel) {
    selectedChannel_ = selectedChannel;
    update();
}

void AllChannelsView::setViewState(double startTime, double windowSeconds, double voltageScale) {
    startTime_ = startTime;
    windowSeconds_ = windowSeconds;
    voltageScale_ = voltageScale;
    update();
}

void AllChannelsView::setTransformMode(spikeviewer::TransformMode transformMode) {
    transformMode_ = transformMode;
    update();
}

void AllChannelsView::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);

    QPainter painter(this);
    painter.fillRect(rect(), QColor::fromRgb(kPlotBackground));
    painter.setRenderHint(QPainter::Antialiasing, true);

    if (!recording_ || displayChannels_.isEmpty()) {
        painter.setPen(QColor::fromRgb(kEmptyText));
        painter.drawText(rect(), Qt::AlignCenter, recording_ ? QStringLiteral("No channels in this tab.") : QStringLiteral("Load an info.rhd to begin."));
        return;
    }

    const QRect plot = insetRect(rect(), 8, 8, 8, 8);
    const int start = std::clamp(static_cast<int>(std::floor(startTime_ * recording_->sampleRate)), 0, recording_->sampleCount);
    const int widthSamples = std::max(1, static_cast<int>(std::llround(windowSeconds_ * recording_->sampleRate)));
    const int end = std::min(start + widthSamples, recording_->sampleCount);
    const int length = end - start;
    if (length <= 0) {
        return;
    }

    const float displayScale = static_cast<float>(std::max(voltageScale_, 1.0e-6));
    const int channelCount = displayChannels_.size();
    const int targetPoints = std::max(400, plot.width() * 2);

    std::vector<float> ptps;
    ptps.reserve(static_cast<std::size_t>(channelCount));
    std::vector<bool> digitalFlags;
    digitalFlags.reserve(static_cast<std::size_t>(channelCount));
    std::vector<DownsampledTrace> traces;
    traces.reserve(static_cast<std::size_t>(channelCount));

    float spacing = 90.0f;
    for (int displayRow = 0; displayRow < channelCount; ++displayRow) {
        const spikeviewer::ChannelInfo& channel = recording_->channels[displayChannels_[displayRow]];
        const bool digitalChannel = spikeviewer::isDigitalChannel(channel);
        const QVector<float> transformed = spikeviewer::extractChannelWindow(
            *recording_, channel, start, end, transformMode_);
        digitalFlags.push_back(digitalChannel);
        if (transformed.isEmpty()) {
            ptps.push_back(0.0f);
            traces.push_back(DownsampledTrace{});
            continue;
        }
        std::vector<float> samples(static_cast<std::size_t>(transformed.size()));
        const float channelScale = digitalChannel
            ? static_cast<float>(std::max(voltageScale_ * 48.0, 24.0))
            : displayScale;
        float minRaw = std::numeric_limits<float>::infinity();
        float maxRaw = -std::numeric_limits<float>::infinity();
        for (int offset = 0; offset < transformed.size(); ++offset) {
            const float rawValue = transformed[offset];
            const float scaled = transformed[offset] * channelScale;
            minRaw = std::min(minRaw, rawValue);
            maxRaw = std::max(maxRaw, rawValue);
            samples[static_cast<std::size_t>(offset)] = scaled;
        }
        ptps.push_back(maxRaw - minRaw);
        traces.push_back(downsampleTrace(samples, targetPoints));
    }

    spacing = std::max(percentileCopy(ptps, 0.75f) * 1.2f, 90.0f);
    const float channelMargin = spacing * 0.75f;
    const float globalMin = -channelMargin;
    const float globalMax = (static_cast<float>(std::max(channelCount - 1, 0)) * spacing) + channelMargin;

    for (int visualIndex = 0; visualIndex < channelCount; ++visualIndex) {
        const DownsampledTrace& trace = traces[static_cast<std::size_t>(visualIndex)];
        if (trace.x.isEmpty()) {
            continue;
        }
        const float offset = static_cast<float>((channelCount - 1 - visualIndex) * spacing);
        QPolygonF polyline;
        const bool digitalChannel = digitalFlags[static_cast<std::size_t>(visualIndex)];
        if (digitalChannel) {
            polyline.reserve((trace.x.size() * 2) + 2);
            auto yForValue = [&](float value) {
                const float yValue = value + offset;
                const float yRatio = normalizeToRange(yValue, globalMin, globalMax);
                return plot.bottom() - (yRatio * plot.height());
            };
            polyline.append(QPointF(plot.left(), yForValue(trace.y.front())));
            for (int i = 1; i < trace.x.size(); ++i) {
                const qreal x = plot.left() + (trace.x[i] * plot.width());
                const qreal previousY = yForValue(trace.y[i - 1]);
                const qreal currentY = yForValue(trace.y[i]);
                polyline.append(QPointF(x, previousY));
                polyline.append(QPointF(x, currentY));
            }
        } else {
            polyline.reserve(trace.x.size());
            for (int i = 0; i < trace.x.size(); ++i) {
                const qreal x = plot.left() + (trace.x[i] * plot.width());
                const float yValue = trace.y[i] + offset;
                const float yRatio = normalizeToRange(yValue, globalMin, globalMax);
                const qreal y = plot.bottom() - (yRatio * plot.height());
                polyline.append(QPointF(x, y));
            }
        }

        QColor color = channelColor(visualIndex, std::max(channelCount, 1));
        color.setAlpha(visualIndex == selectedChannel_ ? 255 : 210);
        QPen pen(color);
        pen.setWidthF(visualIndex == selectedChannel_ ? 1.6 : 0.95);
        pen.setCosmetic(true);
        painter.setPen(pen);
        painter.drawPolyline(polyline);
    }
}

DetailTraceView::DetailTraceView(QWidget* parent)
    : QWidget(parent) {
    setMinimumWidth(260);
}

void DetailTraceView::setRecording(const std::shared_ptr<spikeviewer::RecordingData>& recording) {
    recording_ = recording;
    update();
}

void DetailTraceView::setDisplayChannels(const QVector<int>& displayChannels) {
    displayChannels_ = displayChannels;
    update();
}

void DetailTraceView::setSelectedChannel(int selectedChannel) {
    selectedChannel_ = selectedChannel;
    update();
}

void DetailTraceView::setViewState(double startTime, double windowSeconds, double voltageScale) {
    startTime_ = startTime;
    windowSeconds_ = windowSeconds;
    voltageScale_ = voltageScale;
    update();
}

void DetailTraceView::setTransformMode(spikeviewer::TransformMode transformMode) {
    transformMode_ = transformMode;
    update();
}

void DetailTraceView::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);

    QPainter painter(this);
    painter.fillRect(rect(), QColor::fromRgb(kPlotBackground));
    painter.setRenderHint(QPainter::Antialiasing, true);

    if (!recording_ || selectedChannel_ < 0 || selectedChannel_ >= displayChannels_.size()) {
        painter.setPen(QColor::fromRgb(kEmptyText));
        painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("Select a channel"));
        return;
    }

    const QRect plot = insetRect(rect(), 8, 8, 8, 8);
    const double zoomSeconds = std::max(std::min(windowSeconds_ * 0.25, 0.01), 0.002);
    const int radius = std::max(1, static_cast<int>(std::llround((zoomSeconds * recording_->sampleRate) * 0.5)));
    const int centerSample = static_cast<int>(std::llround((startTime_ + (windowSeconds_ * 0.5)) * recording_->sampleRate));
    const int start = std::max(centerSample - radius, 0);
    const int end = std::min(centerSample + radius, recording_->sampleCount);
    const int length = end - start;
    if (length <= 1) {
        return;
    }

    const spikeviewer::ChannelInfo& channel = recording_->channels[displayChannels_[selectedChannel_]];
    const QVector<float> transformed = spikeviewer::extractChannelWindow(
        *recording_, channel, start, end, transformMode_);
    if (transformed.isEmpty()) {
        painter.setPen(QColor::fromRgb(kEmptyText));
        painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("No samples in this window"));
        return;
    }
    const bool digitalChannel = spikeviewer::isDigitalChannel(channel);
    std::vector<float> samples(static_cast<std::size_t>(transformed.size()));
    std::vector<float> values;
    values.reserve(static_cast<std::size_t>(transformed.size()));
    const float displayScale = static_cast<float>(std::max(voltageScale_, 1.0e-6));

    for (int offset = 0; offset < transformed.size(); ++offset) {
        const float value = transformed[offset];
        samples[static_cast<std::size_t>(offset)] = value;
        values.push_back(value);
    }

    float centerValue = 0.0f;
    float halfRange = 1.0f;
    if (digitalChannel) {
        centerValue = 0.5f;
        halfRange = std::max(0.65f / displayScale, 0.05f);
    } else {
        centerValue = percentileCopy(values, 0.5f);
        std::vector<float> deviations;
        deviations.reserve(values.size());
        for (float value : values) {
            deviations.push_back(std::abs(value - centerValue));
        }
        halfRange = std::max(percentileCopy(deviations, 0.985f) * 1.25f, 10.0f);
        halfRange = std::max(halfRange / displayScale, 1.0f);
    }
    const float minDisplay = centerValue - halfRange;
    const float maxDisplay = centerValue + halfRange;

    QPolygonF polyline;
    const double traceStartTime = static_cast<double>(start) / recording_->sampleRate;
    const double traceEndTime = static_cast<double>(end) / recording_->sampleRate;
    const double centerTime = static_cast<double>(centerSample) / recording_->sampleRate;
    if (digitalChannel) {
        polyline.reserve((transformed.size() * 2) + 2);
        auto yForValue = [&](float value) {
            const float yRatio = normalizeToRange(value, minDisplay, maxDisplay);
            return plot.bottom() - (yRatio * plot.height());
        };
        const qreal firstY = yForValue(transformed.front());
        polyline.append(QPointF(plot.left(), firstY));
        for (int i = 1; i < transformed.size(); ++i) {
            const float ratio = static_cast<float>(i) / static_cast<float>(std::max(transformed.size() - 1, 1));
            const qreal x = plot.left() + (ratio * plot.width());
            const qreal previousY = yForValue(transformed[i - 1]);
            const qreal currentY = yForValue(transformed[i]);
            polyline.append(QPointF(x, previousY));
            polyline.append(QPointF(x, currentY));
        }
    } else {
        const DownsampledTrace trace = downsampleTrace(samples, std::max(500, plot.width() * 2));
        polyline.reserve(trace.x.size());
        for (int i = 0; i < trace.x.size(); ++i) {
            const qreal x = plot.left() + (trace.x[i] * plot.width());
            const float yRatio = normalizeToRange(trace.y[i], minDisplay, maxDisplay);
            const qreal y = plot.bottom() - (yRatio * plot.height());
            polyline.append(QPointF(x, y));
        }
    }

    QColor color = channelColor(selectedChannel_, std::max(displayChannels_.size(), 1));
    QPen pen(color);
    pen.setWidthF(1.35);
    pen.setCosmetic(true);
    painter.setPen(pen);
    painter.drawPolyline(polyline);

    const float cursorRatio = normalizeToRange(static_cast<float>(centerTime), static_cast<float>(traceStartTime), static_cast<float>(traceEndTime));
    const qreal cursorX = plot.left() + (cursorRatio * plot.width());
    painter.setPen(QPen(QColor::fromRgb(kCursor), 1.0));
    painter.drawLine(QPointF(cursorX, plot.top()), QPointF(cursorX, plot.bottom()));
}

OverviewView::OverviewView(QWidget* parent)
    : QWidget(parent) {
    setMinimumWidth(320);
}

void OverviewView::setRecording(const std::shared_ptr<spikeviewer::RecordingData>& recording) {
    recording_ = recording;
    update();
}

void OverviewView::setHeatmap(const std::shared_ptr<spikeviewer::HeatmapResult>& heatmap) {
    heatmap_ = heatmap;
    cacheDirty_ = true;
    update();
}

void OverviewView::clearHeatmap() {
    heatmap_.reset();
    matrixCache_ = QImage();
    cacheDirty_ = true;
    update();
}

void OverviewView::setMode(spikeviewer::OverviewMode mode) {
    if (mode_ == mode) {
        return;
    }
    mode_ = mode;
    cacheDirty_ = true;
    update();
}

void OverviewView::setSelectedChannel(int selectedChannel) {
    selectedChannel_ = selectedChannel;
    update();
}

void OverviewView::setTimeSeconds(double timeSeconds) {
    timeSeconds_ = timeSeconds;
    update();
}

QRect OverviewView::plotRect() const {
    const bool matrixMode = mode_ != spikeviewer::OverviewMode::Population && mode_ != spikeviewer::OverviewMode::Motion;
    return matrixMode ? insetRect(rect(), 8, 8, 36, 8) : insetRect(rect(), 8, 8, 8, 8);
}

void OverviewView::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);

    QPainter painter(this);
    painter.fillRect(rect(), QColor::fromRgb(kPlotBackground));
    painter.setRenderHint(QPainter::Antialiasing, true);

    if (!recording_ || !heatmap_ || !heatmap_->isValid()) {
        painter.setPen(QColor::fromRgb(kEmptyText));
        painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("Overview is computing..."));
        return;
    }

    switch (mode_) {
    case spikeviewer::OverviewMode::Motion:
        drawMotionOverview(painter);
        break;
    case spikeviewer::OverviewMode::Population:
        drawPopulationOverview(painter);
        break;
    case spikeviewer::OverviewMode::Activity:
    case spikeviewer::OverviewMode::Events:
    case spikeviewer::OverviewMode::RMS:
    case spikeviewer::OverviewMode::PeakToPeak:
        drawMatrixOverview(painter);
        break;
    }
}

void OverviewView::mousePressEvent(QMouseEvent* event) {
    if (!heatmap_ || !heatmap_->isValid() || heatmap_->times.isEmpty() || !recording_) {
        return;
    }
    const QRect plot = plotRect();
    if (!plot.contains(event->pos())) {
        return;
    }

    const double minTime = heatmap_->times.first();
    const double maxTime = heatmap_->times.last();
    if (maxTime <= minTime) {
        return;
    }

    const double xRatio = std::clamp(static_cast<double>(event->pos().x() - plot.left()) / std::max(plot.width(), 1), 0.0, 1.0);
    const double targetTime = minTime + ((maxTime - minTime) * xRatio);
    emit timeJumpRequested(targetTime);
}

void OverviewView::drawMatrixOverview(QPainter& painter) {
    ensureMatrixCache();
    if (matrixCache_.isNull() || !heatmap_) {
        return;
    }

    const QRect plot = plotRect();
    painter.drawImage(plot, matrixCache_);

    if (selectedChannel_ >= 0) {
        const int rowIndex = heatmap_->order.indexOf(selectedChannel_);
        if (rowIndex >= 0 && heatmap_->rows > 0) {
            const qreal y = plot.bottom() - (((rowIndex + 0.5) / heatmap_->rows) * plot.height());
            painter.setPen(QPen(QColor::fromRgb(kHighlight), 1.0));
            painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        }
    }

    if (!heatmap_->times.isEmpty()) {
        const float minTime = heatmap_->times.first();
        const float maxTime = heatmap_->times.last();
        const qreal x = plot.left() + (normalizeToRange(static_cast<float>(timeSeconds_), minTime, maxTime) * plot.width());
        painter.setPen(QPen(QColor::fromRgb(kCursor), 1.2));
        painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
    }

    const QRect colorbar = QRect(rect().right() - 22, plot.top(), 10, plot.height());
    QLinearGradient gradient(colorbar.bottomLeft(), colorbar.topLeft());
    const QVector<QPair<float, QColor>> palette = paletteForMode(mode_);
    for (const auto& stop : palette) {
        gradient.setColorAt(stop.first, stop.second);
    }
    painter.fillRect(colorbar, gradient);
    painter.setPen(QColor::fromRgb(kEmptyText));
    painter.drawText(QRect(rect().right() - 48, plot.top(), 44, 16), Qt::AlignRight | Qt::AlignVCenter, QString::number(cacheMax_, 'f', 1));
    painter.drawText(QRect(rect().right() - 48, plot.bottom() - 16, 44, 16), Qt::AlignRight | Qt::AlignVCenter, QString::number(cacheMin_, 'f', 1));
    painter.save();
    painter.translate(rect().right() - 34, plot.center().y());
    painter.rotate(-90);
    painter.drawText(QRect(-70, -8, 140, 16), Qt::AlignCenter, cacheLabel_);
    painter.restore();
}

void OverviewView::drawMotionOverview(QPainter& painter) {
    if (!heatmap_ || heatmap_->times.isEmpty() || heatmap_->motionMatrix.size() < (heatmap_->cols * 3)) {
        return;
    }

    const QRect plot = plotRect();
    QVector<float> xShift(heatmap_->cols);
    QVector<float> yShift(heatmap_->cols);
    QVector<float> drift(heatmap_->cols);
    float maxAbs = 1.0e-6f;
    for (int i = 0; i < heatmap_->cols; ++i) {
        xShift[i] = heatmap_->motionMatrix[i];
        yShift[i] = heatmap_->motionMatrix[heatmap_->cols + i];
        drift[i] = heatmap_->motionMatrix[(2 * heatmap_->cols) + i];
        maxAbs = std::max(maxAbs, std::abs(xShift[i]));
        maxAbs = std::max(maxAbs, std::abs(yShift[i]));
        maxAbs = std::max(maxAbs, std::abs(drift[i]));
    }

    const float scale = std::max(maxAbs * 1.1f, 1.0e-6f);
    QVector<float> xNormalized(xShift.size());
    QVector<float> yNormalized(yShift.size());
    QVector<float> driftNormalized(drift.size());
    for (int i = 0; i < xShift.size(); ++i) {
        xNormalized[i] = xShift[i] / scale;
        yNormalized[i] = yShift[i] / scale;
        driftNormalized[i] = drift[i] / scale;
    }

    painter.setPen(QPen(QColor::fromRgb(kPlotGrid), 1.0));
    painter.drawLine(QPointF(plot.left(), plot.center().y()), QPointF(plot.right(), plot.center().y()));
    drawPolylineSeries(painter, plot, heatmap_->times, xNormalized, QColor(56, 189, 248));
    drawPolylineSeries(painter, plot, heatmap_->times, yNormalized, QColor(249, 115, 22));
    drawPolylineSeries(painter, plot, heatmap_->times, driftNormalized, QColor(34, 197, 94));

    const qreal cursorX = plot.left() + (normalizeToRange(static_cast<float>(timeSeconds_), heatmap_->times.first(), heatmap_->times.last()) * plot.width());
    painter.setPen(QPen(QColor::fromRgb(kCursor), 1.2));
    painter.drawLine(QPointF(cursorX, plot.top()), QPointF(cursorX, plot.bottom()));

    painter.setPen(QColor(56, 189, 248));
    painter.drawText(plot.adjusted(6, 4, -6, -4), Qt::AlignTop | Qt::AlignLeft, QStringLiteral("x"));
    painter.setPen(QColor(249, 115, 22));
    painter.drawText(plot.adjusted(28, 4, -6, -4), Qt::AlignTop | Qt::AlignLeft, QStringLiteral("y"));
    painter.setPen(QColor(34, 197, 94));
    painter.drawText(plot.adjusted(48, 4, -6, -4), Qt::AlignTop | Qt::AlignLeft, QStringLiteral("drift"));
}

void OverviewView::drawPopulationOverview(QPainter& painter) {
    if (!heatmap_ || heatmap_->times.isEmpty()) {
        return;
    }

    const QRect plot = plotRect();
    const QVector<float> activity = normalizeSeries(heatmap_->populationActivity);
    const QVector<float> events = normalizeSeries(heatmap_->populationEvents);
    const QVector<float> common = normalizeSeries(heatmap_->commonModeSeries);

    painter.setPen(QPen(QColor::fromRgb(kPlotGrid), 1.0));
    painter.drawLine(QPointF(plot.left(), plot.center().y()), QPointF(plot.right(), plot.center().y()));
    drawPolylineSeries(painter, plot, heatmap_->times, activity, QColor(245, 158, 11));
    drawPolylineSeries(painter, plot, heatmap_->times, events, QColor(239, 68, 68));
    drawPolylineSeries(painter, plot, heatmap_->times, common, QColor(56, 189, 248));

    const qreal cursorX = plot.left() + (normalizeToRange(static_cast<float>(timeSeconds_), heatmap_->times.first(), heatmap_->times.last()) * plot.width());
    painter.setPen(QPen(QColor::fromRgb(kCursor), 1.2));
    painter.drawLine(QPointF(cursorX, plot.top()), QPointF(cursorX, plot.bottom()));

    painter.setPen(QColor(245, 158, 11));
    painter.drawText(plot.adjusted(6, 4, -6, -4), Qt::AlignTop | Qt::AlignLeft, QStringLiteral("activity"));
    painter.setPen(QColor(239, 68, 68));
    painter.drawText(plot.adjusted(54, 4, -6, -4), Qt::AlignTop | Qt::AlignLeft, QStringLiteral("events"));
    painter.setPen(QColor(56, 189, 248));
    painter.drawText(plot.adjusted(98, 4, -6, -4), Qt::AlignTop | Qt::AlignLeft, QStringLiteral("common"));
}

void OverviewView::ensureMatrixCache() const {
    if (!heatmap_ || !heatmap_->isValid()) {
        matrixCache_ = QImage();
        return;
    }
    const QVector<float>* matrix = matrixForMode(heatmap_, mode_);
    if (matrix == nullptr || matrix->isEmpty()) {
        matrixCache_ = QImage();
        return;
    }
    if (!cacheDirty_ && !matrixCache_.isNull()) {
        return;
    }

    float minValue = std::numeric_limits<float>::infinity();
    float maxValue = -std::numeric_limits<float>::infinity();
    for (float value : *matrix) {
        minValue = std::min(minValue, value);
        maxValue = std::max(maxValue, value);
    }
    if (!std::isfinite(minValue) || !std::isfinite(maxValue) || std::abs(maxValue - minValue) < 1.0e-6f) {
        minValue = 0.0f;
        maxValue = minValue + 1.0f;
    }

    matrixCache_ = QImage(heatmap_->cols, heatmap_->rows, QImage::Format_ARGB32_Premultiplied);
    const QVector<QPair<float, QColor>> palette = paletteForMode(mode_);
    for (int row = 0; row < heatmap_->rows; ++row) {
        for (int col = 0; col < heatmap_->cols; ++col) {
            const int sourceIndex = row * heatmap_->cols + col;
            const float t = normalizeToRange((*matrix)[sourceIndex], minValue, maxValue);
            matrixCache_.setPixelColor(col, heatmap_->rows - 1 - row, gradientColor(palette, t));
        }
    }

    cacheMin_ = minValue;
    cacheMax_ = maxValue;
    cacheLabel_ = colorbarLabel(mode_);
    cacheDirty_ = false;
}
