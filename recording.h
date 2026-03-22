#pragma once

#include <QFile>
#include <QString>
#include <QVector>
#include <memory>

namespace spikeviewer {

constexpr quint32 kIntanMagic = 0xC6912702u;
constexpr float kIntanUvPerBit = 0.195f;

struct ChannelInfo {
    int index = 0;
    QString label;
    int electrodeId = 0;
    float x = 0.0f;
    float y = 0.0f;
};

class MappedInt16Matrix {
public:
    MappedInt16Matrix() = default;
    ~MappedInt16Matrix();

    MappedInt16Matrix(const MappedInt16Matrix&) = delete;
    MappedInt16Matrix& operator=(const MappedInt16Matrix&) = delete;

    bool open(const QString& path, int channelCount, QString* error);
    bool isOpen() const;
    int channelCount() const;
    int sampleCount() const;
    float sampleUv(int sampleIndex, int channelIndex) const;
    const qint16* row(int sampleIndex) const;

private:
    QFile file_;
    uchar* mapped_ = nullptr;
    qint64 byteCount_ = 0;
    int channelCount_ = 0;
    int sampleCount_ = 0;
};

struct RecordingData {
    QString baseDir;
    QString infoPath;
    QString amplifierPath;
    QString timePath;
    double sampleRate = 0.0;
    int sampleCount = 0;
    double durationSeconds = 0.0;
    int channelCount = 0;
    QVector<ChannelInfo> channels;
    std::shared_ptr<MappedInt16Matrix> amplifier;
};

enum class OverviewMode {
    Activity,
    Events,
    RMS,
    PeakToPeak,
    Population,
    Motion,
};

struct HeatmapResult {
    int rows = 0;
    int cols = 0;
    QVector<float> times;
    QVector<int> order;
    QVector<float> activityMatrix;
    QVector<float> eventMatrix;
    QVector<float> rmsMatrix;
    QVector<float> ptpMatrix;
    QVector<float> motionMatrix;
    QVector<float> populationActivity;
    QVector<float> populationEvents;
    QVector<float> commonModeSeries;
    QVector<float> eventThresholds;

    bool isValid() const;
};

std::shared_ptr<RecordingData> loadRecording(const QString& infoPath, QString* error);
QVector<float> estimateEventThresholds(const RecordingData& recording, int windowSamples = 6000, int sampleWindows = 10);
HeatmapResult computeHeatmap(const RecordingData& recording);
QString overviewModeName(OverviewMode mode);

}  // namespace spikeviewer
