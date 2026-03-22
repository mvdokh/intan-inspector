#pragma once

#include "recording.h"

#include <QImage>
#include <QWidget>
#include <memory>

class AllChannelsView : public QWidget {
    Q_OBJECT

public:
    explicit AllChannelsView(QWidget* parent = nullptr);

    void setRecording(const std::shared_ptr<spikeviewer::RecordingData>& recording);
    void setDisplayChannels(const QVector<int>& displayChannels);
    void setSelectedChannel(int selectedChannel);
    void setViewState(double startTime, double windowSeconds, double voltageScale);
    void setTransformMode(spikeviewer::TransformMode transformMode);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    std::shared_ptr<spikeviewer::RecordingData> recording_;
    QVector<int> displayChannels_;
    int selectedChannel_ = -1;
    double startTime_ = 0.0;
    double windowSeconds_ = 0.02;
    double voltageScale_ = 1.0;
    spikeviewer::TransformMode transformMode_ = spikeviewer::TransformMode::Raw;
};

class DetailTraceView : public QWidget {
    Q_OBJECT

public:
    explicit DetailTraceView(QWidget* parent = nullptr);

    void setRecording(const std::shared_ptr<spikeviewer::RecordingData>& recording);
    void setDisplayChannels(const QVector<int>& displayChannels);
    void setSelectedChannel(int selectedChannel);
    void setViewState(double startTime, double windowSeconds, double voltageScale);
    void setTransformMode(spikeviewer::TransformMode transformMode);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    std::shared_ptr<spikeviewer::RecordingData> recording_;
    QVector<int> displayChannels_;
    int selectedChannel_ = -1;
    double startTime_ = 0.0;
    double windowSeconds_ = 0.02;
    double voltageScale_ = 1.0;
    spikeviewer::TransformMode transformMode_ = spikeviewer::TransformMode::Raw;
};

class OverviewView : public QWidget {
    Q_OBJECT

public:
    explicit OverviewView(QWidget* parent = nullptr);

    void setRecording(const std::shared_ptr<spikeviewer::RecordingData>& recording);
    void setHeatmap(const std::shared_ptr<spikeviewer::HeatmapResult>& heatmap);
    void clearHeatmap();
    void setMode(spikeviewer::OverviewMode mode);
    void setSelectedChannel(int selectedChannel);
    void setTimeSeconds(double timeSeconds);

signals:
    void timeJumpRequested(double seconds);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

private:
    QRect plotRect() const;
    void drawMatrixOverview(QPainter& painter);
    void drawMotionOverview(QPainter& painter);
    void drawPopulationOverview(QPainter& painter);
    void ensureMatrixCache() const;

    std::shared_ptr<spikeviewer::RecordingData> recording_;
    std::shared_ptr<spikeviewer::HeatmapResult> heatmap_;
    spikeviewer::OverviewMode mode_ = spikeviewer::OverviewMode::Activity;
    int selectedChannel_ = -1;
    double timeSeconds_ = 0.0;

    mutable bool cacheDirty_ = true;
    mutable QImage matrixCache_;
    mutable float cacheMin_ = 0.0f;
    mutable float cacheMax_ = 1.0f;
    mutable QString cacheLabel_;
};
