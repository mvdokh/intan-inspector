#include "recording.h"

#include <QDataStream>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTextStream>
#include <QXmlStreamReader>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <vector>

namespace spikeviewer {
namespace {

struct HeaderInfo {
    QString version;
    double sampleRate = 0.0;
};

struct DataConfig {
    int channelCount = 0;
    QString timePath;
};

struct SettingsInfo {
    double sampleRate = 0.0;
    QVector<QString> labels;
};

QString findFirst(const QString& baseDir, const QStringList& patterns) {
    QDir dir(baseDir);
    for (const QString& pattern : patterns) {
        const QStringList matches = dir.entryList(QStringList(pattern), QDir::Files, QDir::Name);
        if (!matches.isEmpty()) {
            return dir.absoluteFilePath(matches.first());
        }
    }
    return {};
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

HeaderInfo parseIntanHeader(const QString& path, QString* error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = QStringLiteral("Could not open %1.").arg(QFileInfo(path).fileName());
        }
        return {};
    }
    if (file.size() < 12) {
        if (error) {
            *error = QStringLiteral("%1 is too short to be a valid Intan header.").arg(QFileInfo(path).fileName());
        }
        return {};
    }

    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::LittleEndian);

    quint32 magic = 0;
    qint16 major = 0;
    qint16 minor = 0;
    float sampleRate = 0.0f;
    stream >> magic >> major >> minor >> sampleRate;

    if (magic != kIntanMagic) {
        if (error) {
            *error = QStringLiteral("%1 does not look like an Intan RHD file.").arg(QFileInfo(path).fileName());
        }
        return {};
    }

    HeaderInfo info;
    info.version = QStringLiteral("%1.%2").arg(major).arg(minor);
    info.sampleRate = static_cast<double>(sampleRate);
    return info;
}

DataConfig parseDataConfig(const QString& baseDir) {
    DataConfig config;
    const QString configPath = findFirst(baseDir, {QStringLiteral("*data*.json"), QStringLiteral("*Data*.json")});
    if (configPath.isEmpty()) {
        return config;
    }

    QFile file(configPath);
    if (!file.open(QIODevice::ReadOnly)) {
        return config;
    }

    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isArray()) {
        return config;
    }

    const QDir dir(baseDir);
    for (const QJsonValue& value : document.array()) {
        if (!value.isObject()) {
            continue;
        }
        const QJsonObject object = value.toObject();
        if (object.value(QStringLiteral("filepath")).toString() == QStringLiteral("amplifier.dat")) {
            config.channelCount = object.value(QStringLiteral("channel_count")).toInt(config.channelCount);
        }
        if (object.value(QStringLiteral("name")).toString() == QStringLiteral("master")) {
            const QString timeFile = object.value(QStringLiteral("filepath")).toString();
            if (!timeFile.isEmpty()) {
                config.timePath = dir.absoluteFilePath(timeFile);
            }
        }
    }

    return config;
}

SettingsInfo parseSettingsXml(const QString& path) {
    SettingsInfo info;

    QFile file(path);
    if (!file.exists() || !file.open(QIODevice::ReadOnly)) {
        return info;
    }

    QXmlStreamReader xml(&file);
    while (!xml.atEnd()) {
        xml.readNext();
        if (!xml.isStartElement()) {
            continue;
        }

        if (info.sampleRate <= 0.0) {
            const QString sampleRate = xml.attributes().value(QStringLiteral("SampleRateHertz")).toString();
            bool ok = false;
            const double parsed = sampleRate.toDouble(&ok);
            if (ok) {
                info.sampleRate = parsed;
            }
        }

        if (xml.name() == QStringLiteral("Channel")) {
            const QString enabled = xml.attributes().value(QStringLiteral("Enabled")).toString();
            if (!enabled.isEmpty() && enabled != QStringLiteral("True")) {
                continue;
            }
            QString label = xml.attributes().value(QStringLiteral("CustomChannelName")).toString();
            if (label.isEmpty()) {
                label = xml.attributes().value(QStringLiteral("NativeChannelName")).toString();
            }
            if (label.isEmpty()) {
                label = QStringLiteral("Ch %1").arg(info.labels.size() + 1);
            }
            info.labels.push_back(label);
        }
    }

    return info;
}

QVector<ChannelInfo> parseElectrodeCfg(const QString& path, int channelCount, const QVector<QString>& labels) {
    QVector<ChannelInfo> channels;
    channels.resize(channelCount);

    for (int index = 0; index < channelCount; ++index) {
        ChannelInfo info;
        info.index = index;
        info.label = index < labels.size() ? labels[index] : QStringLiteral("Ch %1").arg(index + 1);
        info.electrodeId = index + 1;
        info.x = static_cast<float>(index);
        info.y = 0.0f;
        channels[index] = info;
    }

    QFile file(path);
    if (!file.exists() || !file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return channels;
    }

    QTextStream stream(&file);
    bool firstLine = true;
    while (!stream.atEnd()) {
        const QString rawLine = stream.readLine().trimmed();
        if (rawLine.isEmpty()) {
            continue;
        }
        if (firstLine) {
            firstLine = false;
            continue;
        }

        const QString normalized = QString(rawLine).replace(',', ' ');
        const QStringList parts = normalized.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        if (parts.size() < 4) {
            continue;
        }

        bool okElectrode = false;
        bool okChannel = false;
        bool okX = false;
        bool okY = false;

        const int electrodeId = parts[0].toInt(&okElectrode);
        const int channelNumber = parts[1].toInt(&okChannel);
        const float x = parts[2].toFloat(&okX);
        const float y = parts[3].toFloat(&okY);

        if (!okElectrode || !okChannel || !okX || !okY) {
            continue;
        }

        const int index = channelNumber - 1;
        if (index < 0 || index >= channelCount) {
            continue;
        }

        channels[index].electrodeId = electrodeId;
        channels[index].x = x;
        channels[index].y = y;
    }

    return channels;
}

}  // namespace

MappedInt16Matrix::~MappedInt16Matrix() {
    if (mapped_ != nullptr) {
        file_.unmap(mapped_);
    }
}

bool MappedInt16Matrix::open(const QString& path, int channelCount, QString* error) {
    if (mapped_ != nullptr) {
        file_.unmap(mapped_);
        mapped_ = nullptr;
    }

    file_.setFileName(path);
    if (!file_.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = QStringLiteral("Could not open amplifier.dat.");
        }
        return false;
    }

    byteCount_ = file_.size();
    if (byteCount_ <= 0 || (byteCount_ % static_cast<qint64>(sizeof(qint16))) != 0) {
        if (error) {
            *error = QStringLiteral("amplifier.dat has an invalid byte size.");
        }
        return false;
    }
    if (channelCount <= 0) {
        if (error) {
            *error = QStringLiteral("Detected channel count is invalid.");
        }
        return false;
    }

    const qint64 valueCount = byteCount_ / static_cast<qint64>(sizeof(qint16));
    if ((valueCount % channelCount) != 0) {
        if (error) {
            *error = QStringLiteral("amplifier.dat size is not divisible by the detected channel count.");
        }
        return false;
    }

    mapped_ = file_.map(0, byteCount_);
    if (mapped_ == nullptr) {
        if (error) {
            *error = QStringLiteral("Could not memory-map amplifier.dat.");
        }
        return false;
    }

    channelCount_ = channelCount;
    sampleCount_ = static_cast<int>(valueCount / channelCount);
    return true;
}

bool MappedInt16Matrix::isOpen() const {
    return mapped_ != nullptr;
}

int MappedInt16Matrix::channelCount() const {
    return channelCount_;
}

int MappedInt16Matrix::sampleCount() const {
    return sampleCount_;
}

float MappedInt16Matrix::sampleUv(int sampleIndex, int channelIndex) const {
    return static_cast<float>(row(sampleIndex)[channelIndex]) * kIntanUvPerBit;
}

const qint16* MappedInt16Matrix::row(int sampleIndex) const {
    return reinterpret_cast<const qint16*>(mapped_) + (static_cast<qint64>(sampleIndex) * channelCount_);
}

bool HeatmapResult::isValid() const {
    return rows > 0 && cols > 0 && times.size() == cols;
}

std::shared_ptr<RecordingData> loadRecording(const QString& infoPath, QString* error) {
    const QFileInfo infoFile(infoPath);
    if (!infoFile.exists()) {
        if (error) {
            *error = QStringLiteral("%1 was not found.").arg(infoPath);
        }
        return {};
    }

    QString headerError;
    const HeaderInfo header = parseIntanHeader(infoPath, &headerError);
    if (!headerError.isEmpty()) {
        if (error) {
            *error = headerError;
        }
        return {};
    }

    const QString baseDir = infoFile.absolutePath();
    const DataConfig config = parseDataConfig(baseDir);
    const SettingsInfo settings = parseSettingsXml(QDir(baseDir).absoluteFilePath(QStringLiteral("settings.xml")));

    const QString amplifierPath = QDir(baseDir).absoluteFilePath(QStringLiteral("amplifier.dat"));
    if (!QFileInfo::exists(amplifierPath)) {
        if (error) {
            *error = QStringLiteral("amplifier.dat was not found next to info.rhd.");
        }
        return {};
    }

    const int channelCount = std::max(config.channelCount, std::max(settings.labels.size(), 32));
    const double sampleRate = settings.sampleRate > 0.0 ? settings.sampleRate : header.sampleRate;
    if (sampleRate <= 0.0) {
        if (error) {
            *error = QStringLiteral("Unable to determine the sample rate from info.rhd or settings.xml.");
        }
        return {};
    }

    auto amplifier = std::make_shared<MappedInt16Matrix>();
    QString mapError;
    if (!amplifier->open(amplifierPath, channelCount, &mapError)) {
        if (error) {
            *error = mapError;
        }
        return {};
    }

    auto recording = std::make_shared<RecordingData>();
    recording->baseDir = baseDir;
    recording->infoPath = infoPath;
    recording->amplifierPath = amplifierPath;
    recording->timePath = !config.timePath.isEmpty() ? config.timePath : findFirst(baseDir, {QStringLiteral("time.dat")});
    recording->sampleRate = sampleRate;
    recording->sampleCount = amplifier->sampleCount();
    recording->durationSeconds = static_cast<double>(recording->sampleCount) / sampleRate;
    recording->channelCount = channelCount;
    recording->channels = parseElectrodeCfg(QDir(baseDir).absoluteFilePath(QStringLiteral("electrode.cfg")), channelCount, settings.labels);
    recording->amplifier = amplifier;
    return recording;
}

QVector<float> estimateEventThresholds(const RecordingData& recording, int windowSamples, int sampleWindows) {
    QVector<float> thresholds(recording.channelCount, -20.0f);
    if (recording.sampleCount <= 0 || !recording.amplifier || !recording.amplifier->isOpen()) {
        return thresholds;
    }

    windowSamples = std::max(256, std::min(windowSamples, recording.sampleCount));
    sampleWindows = std::max(1, sampleWindows);

    const int maxStart = std::max(recording.sampleCount - windowSamples, 0);
    std::vector<std::vector<float>> sigmaPerChannel(static_cast<std::size_t>(recording.channelCount));
    std::vector<float> scratch(static_cast<std::size_t>(windowSamples));
    std::vector<float> centered(static_cast<std::size_t>(windowSamples));

    for (int windowIndex = 0; windowIndex < sampleWindows; ++windowIndex) {
        const int start = (sampleWindows == 1)
            ? 0
            : static_cast<int>(std::llround((static_cast<double>(windowIndex) * maxStart) / (sampleWindows - 1)));
        for (int channel = 0; channel < recording.channelCount; ++channel) {
            for (int offset = 0; offset < windowSamples; ++offset) {
                scratch[static_cast<std::size_t>(offset)] = recording.amplifier->sampleUv(start + offset, channel);
            }
            const float median = medianCopy(scratch);
            for (int offset = 0; offset < windowSamples; ++offset) {
                centered[static_cast<std::size_t>(offset)] = std::abs(scratch[static_cast<std::size_t>(offset)] - median);
            }
            const float mad = medianCopy(centered);
            sigmaPerChannel[static_cast<std::size_t>(channel)].push_back(mad / 0.6745f);
        }
    }

    for (int channel = 0; channel < recording.channelCount; ++channel) {
        float sigma = medianCopy(sigmaPerChannel[static_cast<std::size_t>(channel)]);
        sigma = std::max(sigma, 2.5f);
        thresholds[channel] = -4.0f * sigma;
    }

    return thresholds;
}

HeatmapResult computeHeatmap(const RecordingData& recording) {
    HeatmapResult result;
    if (recording.sampleCount <= 0 || recording.channelCount <= 0 || !recording.amplifier || !recording.amplifier->isOpen()) {
        return result;
    }

    result.eventThresholds = estimateEventThresholds(recording);

    QVector<int> order(recording.channelCount);
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](int left, int right) {
        const ChannelInfo& a = recording.channels[left];
        const ChannelInfo& b = recording.channels[right];
        if (a.y == b.y) {
            return a.x < b.x;
        }
        return a.y < b.y;
    });

    const int binCount = std::max(180, std::min(420, static_cast<int>(recording.durationSeconds / 2.0)));
    QVector<int> edges(binCount + 1);
    for (int index = 0; index <= binCount; ++index) {
        edges[index] = static_cast<int>(std::llround((static_cast<double>(index) * recording.sampleCount) / binCount));
    }

    result.rows = recording.channelCount;
    result.cols = binCount;
    result.order = order;
    result.times.resize(binCount);
    result.activityMatrix.fill(0.0f, result.rows * result.cols);
    result.eventMatrix.fill(0.0f, result.rows * result.cols);
    result.rmsMatrix.fill(0.0f, result.rows * result.cols);
    result.ptpMatrix.fill(0.0f, result.rows * result.cols);
    result.motionMatrix.fill(0.0f, 3 * result.cols);
    result.populationActivity.fill(0.0f, result.cols);
    result.populationEvents.fill(0.0f, result.cols);
    result.commonModeSeries.fill(0.0f, result.cols);

    QVector<float> xCoords(recording.channelCount);
    QVector<float> yCoords(recording.channelCount);
    for (int channel = 0; channel < recording.channelCount; ++channel) {
        xCoords[channel] = recording.channels[channel].x;
        yCoords[channel] = recording.channels[channel].y;
    }

    std::vector<float> scratch;
    QVector<float> medians(recording.channelCount);
    QVector<double> sumAbs(recording.channelCount);
    QVector<double> sumSq(recording.channelCount);
    QVector<double> minCentered(recording.channelCount);
    QVector<double> maxCentered(recording.channelCount);
    QVector<float> previousCentered(recording.channelCount);
    QVector<float> activity(recording.channelCount);
    QVector<float> events(recording.channelCount);
    QVector<float> rms(recording.channelCount);
    QVector<float> ptp(recording.channelCount);
    QVector<float> xCenters(binCount);
    QVector<float> yCenters(binCount);

    for (int binIndex = 0; binIndex < binCount; ++binIndex) {
        const int start = edges[binIndex];
        const int end = edges[binIndex + 1];
        const int length = end - start;
        result.times[binIndex] = static_cast<float>((start + end) * 0.5 / recording.sampleRate);
        if (length <= 0) {
            continue;
        }

        scratch.resize(static_cast<std::size_t>(length));
        for (int channel = 0; channel < recording.channelCount; ++channel) {
            for (int offset = 0; offset < length; ++offset) {
                scratch[static_cast<std::size_t>(offset)] = recording.amplifier->sampleUv(start + offset, channel);
            }
            medians[channel] = medianCopy(scratch);
        }

        std::fill(sumAbs.begin(), sumAbs.end(), 0.0);
        std::fill(sumSq.begin(), sumSq.end(), 0.0);
        std::fill(minCentered.begin(), minCentered.end(), std::numeric_limits<double>::infinity());
        std::fill(maxCentered.begin(), maxCentered.end(), -std::numeric_limits<double>::infinity());
        std::fill(previousCentered.begin(), previousCentered.end(), 0.0f);
        std::fill(activity.begin(), activity.end(), 0.0f);
        std::fill(events.begin(), events.end(), 0.0f);
        std::fill(rms.begin(), rms.end(), 0.0f);
        std::fill(ptp.begin(), ptp.end(), 0.0f);

        double commonModeSq = 0.0;
        for (int sampleIndex = start; sampleIndex < end; ++sampleIndex) {
            const qint16* row = recording.amplifier->row(sampleIndex);
            double sampleMean = 0.0;
            for (int channel = 0; channel < recording.channelCount; ++channel) {
                const float raw = static_cast<float>(row[channel]) * kIntanUvPerBit;
                const float centered = raw - medians[channel];
                sumAbs[channel] += std::abs(raw);
                sumSq[channel] += centered * centered;
                minCentered[channel] = std::min(minCentered[channel], static_cast<double>(centered));
                maxCentered[channel] = std::max(maxCentered[channel], static_cast<double>(centered));
                if (sampleIndex > start && centered < result.eventThresholds[channel] && previousCentered[channel] >= result.eventThresholds[channel]) {
                    events[channel] += 1.0f;
                }
                previousCentered[channel] = centered;
                sampleMean += centered;
            }
            sampleMean /= recording.channelCount;
            commonModeSq += sampleMean * sampleMean;
        }

        float populationActivity = 0.0f;
        float populationEvents = 0.0f;
        for (int channel = 0; channel < recording.channelCount; ++channel) {
            activity[channel] = static_cast<float>(sumAbs[channel] / length);
            rms[channel] = static_cast<float>(std::sqrt(sumSq[channel] / length));
            ptp[channel] = static_cast<float>(maxCentered[channel] - minCentered[channel]);
            populationActivity += activity[channel];
            populationEvents += events[channel];
        }
        populationActivity /= std::max(recording.channelCount, 1);

        const float baseline = [&]() {
            std::vector<float> activityCopy(activity.begin(), activity.end());
            return medianCopy(activityCopy);
        }();

        double weightSum = 0.0;
        double xCenter = 0.0;
        double yCenter = 0.0;
        for (int channel = 0; channel < recording.channelCount; ++channel) {
            double weight = std::max(0.0f, activity[channel] - baseline);
            if (weight <= 1e-6) {
                weight = std::max(0.0f, activity[channel]);
            }
            weightSum += weight;
            xCenter += weight * xCoords[channel];
            yCenter += weight * yCoords[channel];
        }
        if (weightSum > 1e-6) {
            xCenters[binIndex] = static_cast<float>(xCenter / weightSum);
            yCenters[binIndex] = static_cast<float>(yCenter / weightSum);
        }

        result.populationActivity[binIndex] = populationActivity;
        result.populationEvents[binIndex] = populationEvents;
        result.commonModeSeries[binIndex] = static_cast<float>(std::sqrt(commonModeSq / length));

        for (int rowIndex = 0; rowIndex < result.rows; ++rowIndex) {
            const int sourceChannel = order[rowIndex];
            const int matrixIndex = rowIndex * result.cols + binIndex;
            result.activityMatrix[matrixIndex] = activity[sourceChannel];
            result.eventMatrix[matrixIndex] = events[sourceChannel];
            result.rmsMatrix[matrixIndex] = rms[sourceChannel];
            result.ptpMatrix[matrixIndex] = ptp[sourceChannel];
        }
    }

    std::vector<float> xCopy(xCenters.begin(), xCenters.end());
    std::vector<float> yCopy(yCenters.begin(), yCenters.end());
    const float xMedian = medianCopy(xCopy);
    const float yMedian = medianCopy(yCopy);
    for (int index = 0; index < binCount; ++index) {
        const float xShift = xCenters[index] - xMedian;
        const float yShift = yCenters[index] - yMedian;
        result.motionMatrix[index] = xShift;
        result.motionMatrix[result.cols + index] = yShift;
        result.motionMatrix[(2 * result.cols) + index] = std::sqrt((xShift * xShift) + (yShift * yShift));
    }

    return result;
}

QString overviewModeName(OverviewMode mode) {
    switch (mode) {
    case OverviewMode::Activity:
        return QStringLiteral("Activity");
    case OverviewMode::Events:
        return QStringLiteral("Events");
    case OverviewMode::RMS:
        return QStringLiteral("RMS");
    case OverviewMode::PeakToPeak:
        return QStringLiteral("Peak-to-Peak");
    case OverviewMode::Population:
        return QStringLiteral("Population");
    case OverviewMode::Motion:
        return QStringLiteral("Motion");
    }
    return QStringLiteral("Activity");
}

}  // namespace spikeviewer
