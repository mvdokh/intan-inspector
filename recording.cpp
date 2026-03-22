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

constexpr double kPi = 3.14159265358979323846;

struct HeaderInfo {
    QString version;
    double sampleRate = 0.0;
};

struct DigitalChannelDescriptor {
    QString path;
    int bitIndex = 0;
    QString label;
};

struct DataConfig {
    int channelCount = 0;
    QString masterPath;
    QString masterFormat;
    QVector<DigitalChannelDescriptor> digitalChannels;
};

struct SettingsInfo {
    double sampleRate = 0.0;
    QVector<QString> labels;
};

struct BiquadCoefficients {
    double b0 = 1.0;
    double b1 = 0.0;
    double b2 = 0.0;
    double a1 = 0.0;
    double a2 = 0.0;
    bool valid = false;
};

void appendCandidate(QVector<int>* candidates, int value) {
    if (candidates == nullptr || value <= 0) {
        return;
    }
    if (!candidates->contains(value)) {
        candidates->push_back(value);
    }
}

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

int bytesPerSampleForFormat(const QString& format) {
    const QString normalized = format.trimmed().toLower();
    if (normalized == QStringLiteral("int16")
        || normalized == QStringLiteral("uint16")
        || normalized == QStringLiteral("uint16_length")
        || normalized == QStringLiteral("int16_length")) {
        return 2;
    }
    if (normalized == QStringLiteral("int32")
        || normalized == QStringLiteral("uint32")
        || normalized == QStringLiteral("int32_length")
        || normalized == QStringLiteral("uint32_length")) {
        return 4;
    }
    return 0;
}

QString normalizeDigitalLabel(const QString& name, int bitIndex) {
    QString label = QStringLiteral("DIGITAL IN %1").arg(bitIndex);
    const QString trimmed = name.trimmed();
    if (!trimmed.isEmpty() && trimmed.compare(QStringLiteral("master"), Qt::CaseInsensitive) != 0) {
        label += QStringLiteral(" | %1").arg(trimmed);
    }
    return label;
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
        const QString relativePath = object.value(QStringLiteral("filepath")).toString();
        const QString absolutePath = relativePath.isEmpty() ? QString() : dir.absoluteFilePath(relativePath);
        const QString dataType = object.value(QStringLiteral("data_type")).toString();
        const QString name = object.value(QStringLiteral("name")).toString();
        const QString format = object.value(QStringLiteral("format")).toString();

        if (QFileInfo(relativePath).fileName() == QStringLiteral("amplifier.dat")) {
            config.channelCount = object.value(QStringLiteral("channel_count")).toInt(config.channelCount);
            continue;
        }

        if (name.compare(QStringLiteral("master"), Qt::CaseInsensitive) == 0 && !absolutePath.isEmpty()) {
            config.masterPath = absolutePath;
            config.masterFormat = format;
            continue;
        }

        if (!object.contains(QStringLiteral("channel"))) {
            continue;
        }

        bool okChannel = false;
        const int bitIndex = object.value(QStringLiteral("channel")).toVariant().toInt(&okChannel);
        if (!okChannel || bitIndex < 0 || absolutePath.isEmpty()) {
            continue;
        }

        const bool looksDigital = dataType.startsWith(QStringLiteral("digital"), Qt::CaseInsensitive)
            || absolutePath.contains(QStringLiteral("digital"), Qt::CaseInsensitive)
            || format.startsWith(QStringLiteral("uint16"), Qt::CaseInsensitive);
        if (!looksDigital) {
            continue;
        }

        bool duplicate = false;
        for (const DigitalChannelDescriptor& descriptor : config.digitalChannels) {
            if (descriptor.path == absolutePath && descriptor.bitIndex == bitIndex) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            continue;
        }

        DigitalChannelDescriptor descriptor;
        descriptor.path = absolutePath;
        descriptor.bitIndex = bitIndex;
        descriptor.label = normalizeDigitalLabel(name, bitIndex);
        config.digitalChannels.push_back(descriptor);
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
        info.sourceKind = ChannelInfo::SourceKind::Amplifier;
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

int inferElectrodeChannelCount(const QString& path) {
    QFile file(path);
    if (!file.exists() || !file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return 0;
    }

    QTextStream stream(&file);
    bool firstLine = true;
    int maxChannelNumber = 0;
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
        if (parts.size() < 2) {
            continue;
        }

        bool okChannel = false;
        const int channelNumber = parts[1].toInt(&okChannel);
        if (okChannel) {
            maxChannelNumber = std::max(maxChannelNumber, channelNumber);
        }
    }

    return maxChannelNumber;
}

int inferChannelCount(
    const QString& amplifierPath,
    const QString& masterPath,
    int masterBytesPerSample,
    const DataConfig& config,
    const SettingsInfo& settings,
    const QString& electrodeCfgPath,
    QString* error) {
    const QFileInfo amplifierInfo(amplifierPath);
    const qint64 amplifierBytes = amplifierInfo.size();
    if (amplifierBytes <= 0 || (amplifierBytes % static_cast<qint64>(sizeof(qint16))) != 0) {
        if (error) {
            *error = QStringLiteral("amplifier.dat has an invalid byte size.");
        }
        return 0;
    }

    const qint64 amplifierValueCount = amplifierBytes / static_cast<qint64>(sizeof(qint16));
    const int electrodeChannelCount = inferElectrodeChannelCount(electrodeCfgPath);
    int timeInferredChannelCount = 0;
    if (!masterPath.isEmpty() && QFileInfo::exists(masterPath) && masterBytesPerSample > 0) {
        const qint64 masterBytes = QFileInfo(masterPath).size();
        if (masterBytes > 0 && (masterBytes % masterBytesPerSample) == 0) {
            const qint64 masterSampleCount = masterBytes / masterBytesPerSample;
            if (masterSampleCount > 0 && (amplifierValueCount % masterSampleCount) == 0) {
                timeInferredChannelCount = static_cast<int>(amplifierValueCount / masterSampleCount);
            }
        }
    }

    QVector<int> candidates;
    appendCandidate(&candidates, timeInferredChannelCount);
    appendCandidate(&candidates, config.channelCount);
    appendCandidate(&candidates, electrodeChannelCount);
    appendCandidate(&candidates, settings.labels.size());
    appendCandidate(&candidates, 32);

    for (int candidate : candidates) {
        if ((amplifierValueCount % candidate) == 0) {
            return candidate;
        }
    }

    if (error) {
        QStringList parts;
        if (timeInferredChannelCount > 0) {
            parts.push_back(QStringLiteral("master=%1").arg(timeInferredChannelCount));
        }
        if (config.channelCount > 0) {
            parts.push_back(QStringLiteral("data.json=%1").arg(config.channelCount));
        }
        if (electrodeChannelCount > 0) {
            parts.push_back(QStringLiteral("electrode.cfg=%1").arg(electrodeChannelCount));
        }
        if (!settings.labels.isEmpty()) {
            parts.push_back(QStringLiteral("settings.xml=%1").arg(settings.labels.size()));
        }
        *error = QStringLiteral(
            "Could not infer a valid amplifier channel count. amplifier.dat contains %1 int16 values; candidates were [%2].")
                     .arg(amplifierValueCount)
                     .arg(parts.join(QStringLiteral(", ")));
    }
    return 0;
}

double clampCutoff(double sampleRate, double cutoff) {
    return std::clamp(cutoff, 1.0, sampleRate * 0.45);
}

BiquadCoefficients makeLowpass(double sampleRate, double cutoffHz) {
    BiquadCoefficients coefficients;
    if (sampleRate <= 0.0) {
        return coefficients;
    }

    const double cutoff = clampCutoff(sampleRate, cutoffHz);
    const double q = std::sqrt(0.5);
    const double omega = 2.0 * kPi * cutoff / sampleRate;
    const double sinOmega = std::sin(omega);
    const double cosOmega = std::cos(omega);
    const double alpha = sinOmega / (2.0 * q);
    const double a0 = 1.0 + alpha;
    if (std::abs(a0) < 1.0e-12) {
        return coefficients;
    }

    coefficients.b0 = ((1.0 - cosOmega) * 0.5) / a0;
    coefficients.b1 = (1.0 - cosOmega) / a0;
    coefficients.b2 = ((1.0 - cosOmega) * 0.5) / a0;
    coefficients.a1 = (-2.0 * cosOmega) / a0;
    coefficients.a2 = (1.0 - alpha) / a0;
    coefficients.valid = true;
    return coefficients;
}

BiquadCoefficients makeHighpass(double sampleRate, double cutoffHz) {
    BiquadCoefficients coefficients;
    if (sampleRate <= 0.0) {
        return coefficients;
    }

    const double cutoff = clampCutoff(sampleRate, cutoffHz);
    const double q = std::sqrt(0.5);
    const double omega = 2.0 * kPi * cutoff / sampleRate;
    const double sinOmega = std::sin(omega);
    const double cosOmega = std::cos(omega);
    const double alpha = sinOmega / (2.0 * q);
    const double a0 = 1.0 + alpha;
    if (std::abs(a0) < 1.0e-12) {
        return coefficients;
    }

    coefficients.b0 = ((1.0 + cosOmega) * 0.5) / a0;
    coefficients.b1 = (-(1.0 + cosOmega)) / a0;
    coefficients.b2 = ((1.0 + cosOmega) * 0.5) / a0;
    coefficients.a1 = (-2.0 * cosOmega) / a0;
    coefficients.a2 = (1.0 - alpha) / a0;
    coefficients.valid = true;
    return coefficients;
}

BiquadCoefficients makeNotch(double sampleRate, double centerHz, double q) {
    BiquadCoefficients coefficients;
    if (sampleRate <= 0.0) {
        return coefficients;
    }

    const double cutoff = clampCutoff(sampleRate, centerHz);
    const double omega = 2.0 * kPi * cutoff / sampleRate;
    const double sinOmega = std::sin(omega);
    const double cosOmega = std::cos(omega);
    const double alpha = sinOmega / (2.0 * q);
    const double a0 = 1.0 + alpha;
    if (std::abs(a0) < 1.0e-12) {
        return coefficients;
    }

    coefficients.b0 = 1.0 / a0;
    coefficients.b1 = (-2.0 * cosOmega) / a0;
    coefficients.b2 = 1.0 / a0;
    coefficients.a1 = (-2.0 * cosOmega) / a0;
    coefficients.a2 = (1.0 - alpha) / a0;
    coefficients.valid = true;
    return coefficients;
}

void processBiquad(QVector<float>* samples, const BiquadCoefficients& coefficients) {
    if (samples == nullptr || !coefficients.valid || samples->isEmpty()) {
        return;
    }

    double z1 = 0.0;
    double z2 = 0.0;
    for (float& sample : *samples) {
        const double x = static_cast<double>(sample);
        const double y = (coefficients.b0 * x) + z1;
        z1 = (coefficients.b1 * x) - (coefficients.a1 * y) + z2;
        z2 = (coefficients.b2 * x) - (coefficients.a2 * y);
        sample = static_cast<float>(y);
    }
}

void applyZeroPhaseBiquad(QVector<float>* samples, const BiquadCoefficients& coefficients) {
    if (samples == nullptr || samples->size() < 3 || !coefficients.valid) {
        return;
    }
    processBiquad(samples, coefficients);
    std::reverse(samples->begin(), samples->end());
    processBiquad(samples, coefficients);
    std::reverse(samples->begin(), samples->end());
}

int recommendedPaddingSamples(double sampleRate, TransformMode mode) {
    if (mode == TransformMode::Raw) {
        return 0;
    }
    if (sampleRate <= 0.0) {
        return 256;
    }
    return std::clamp(static_cast<int>(std::llround(sampleRate * 0.02)), 128, 4096);
}

void applyTransformInPlace(QVector<float>* samples, double sampleRate, TransformMode mode) {
    if (samples == nullptr || samples->isEmpty() || mode == TransformMode::Raw) {
        return;
    }

    switch (mode) {
    case TransformMode::Raw:
        return;
    case TransformMode::Highpass300:
        applyZeroPhaseBiquad(samples, makeHighpass(sampleRate, 300.0));
        return;
    case TransformMode::Bandpass300To6000:
        applyZeroPhaseBiquad(samples, makeHighpass(sampleRate, 300.0));
        applyZeroPhaseBiquad(samples, makeLowpass(sampleRate, std::min(6000.0, sampleRate * 0.45)));
        return;
    case TransformMode::Bandpass500To3000:
        applyZeroPhaseBiquad(samples, makeHighpass(sampleRate, 500.0));
        applyZeroPhaseBiquad(samples, makeLowpass(sampleRate, std::min(3000.0, sampleRate * 0.45)));
        return;
    case TransformMode::Lowpass250:
        applyZeroPhaseBiquad(samples, makeLowpass(sampleRate, 250.0));
        return;
    case TransformMode::Notch60:
        applyZeroPhaseBiquad(samples, makeNotch(sampleRate, 60.0, 30.0));
        return;
    }
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
            *error = QStringLiteral("Detected amplifier channel count is invalid.");
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

MappedUInt16Vector::~MappedUInt16Vector() {
    if (mapped_ != nullptr) {
        file_.unmap(mapped_);
    }
}

bool MappedUInt16Vector::open(const QString& path, QString* error) {
    if (mapped_ != nullptr) {
        file_.unmap(mapped_);
        mapped_ = nullptr;
    }

    file_.setFileName(path);
    if (!file_.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = QStringLiteral("Could not open %1.").arg(QFileInfo(path).fileName());
        }
        return false;
    }

    byteCount_ = file_.size();
    if (byteCount_ <= 0 || (byteCount_ % static_cast<qint64>(sizeof(quint16))) != 0) {
        if (error) {
            *error = QStringLiteral("%1 has an invalid byte size.").arg(QFileInfo(path).fileName());
        }
        return false;
    }

    mapped_ = file_.map(0, byteCount_);
    if (mapped_ == nullptr) {
        if (error) {
            *error = QStringLiteral("Could not memory-map %1.").arg(QFileInfo(path).fileName());
        }
        return false;
    }

    sampleCount_ = static_cast<int>(byteCount_ / static_cast<qint64>(sizeof(quint16)));
    return true;
}

bool MappedUInt16Vector::isOpen() const {
    return mapped_ != nullptr;
}

int MappedUInt16Vector::sampleCount() const {
    return sampleCount_;
}

quint16 MappedUInt16Vector::sampleValue(int sampleIndex) const {
    return reinterpret_cast<const quint16*>(mapped_)[sampleIndex];
}

bool HeatmapResult::isValid() const {
    return rows > 0 && cols > 0 && times.size() == cols;
}

bool isDigitalChannel(const ChannelInfo& channel) {
    return channel.sourceKind == ChannelInfo::SourceKind::DigitalBit;
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
    const QString masterPath = !config.masterPath.isEmpty() ? config.masterPath : findFirst(baseDir, {QStringLiteral("time.dat")});
    const int masterBytesPerSample = !config.masterFormat.isEmpty()
        ? bytesPerSampleForFormat(config.masterFormat)
        : (QFileInfo(masterPath).fileName() == QStringLiteral("time.dat") ? 4 : 2);
    const QString electrodeCfgPath = QDir(baseDir).absoluteFilePath(QStringLiteral("electrode.cfg"));
    const QString amplifierPath = QDir(baseDir).absoluteFilePath(QStringLiteral("amplifier.dat"));

    if (!QFileInfo::exists(amplifierPath)) {
        if (error) {
            *error = QStringLiteral("amplifier.dat was not found next to info.rhd.");
        }
        return {};
    }

    QString channelError;
    const int channelCount = inferChannelCount(amplifierPath, masterPath, masterBytesPerSample, config, settings, electrodeCfgPath, &channelError);
    if (channelCount <= 0) {
        if (error) {
            *error = channelError;
        }
        return {};
    }

    const double sampleRate = settings.sampleRate > 0.0 ? settings.sampleRate : header.sampleRate;
    if (sampleRate <= 0.0) {
        if (error) {
            *error = QStringLiteral("Unable to determine the sample rate from info.rhd or settings.xml.");
        }
        return {};
    }

    auto amplifier = std::make_shared<MappedInt16Matrix>();
    QString amplifierError;
    if (!amplifier->open(amplifierPath, channelCount, &amplifierError)) {
        if (error) {
            *error = amplifierError;
        }
        return {};
    }

    std::shared_ptr<MappedUInt16Vector> digitalIn;
    QString digitalPath;
    if (!config.digitalChannels.isEmpty()) {
        digitalPath = config.digitalChannels.first().path;
        if (QFileInfo::exists(digitalPath)) {
            digitalIn = std::make_shared<MappedUInt16Vector>();
            QString digitalError;
            if (!digitalIn->open(digitalPath, &digitalError)) {
                if (error) {
                    *error = digitalError;
                }
                return {};
            }
        }
    }

    auto recording = std::make_shared<RecordingData>();
    recording->baseDir = baseDir;
    recording->infoPath = infoPath;
    recording->amplifierPath = amplifierPath;
    recording->digitalInPath = digitalPath;
    recording->timePath = masterPath;
    recording->sampleRate = sampleRate;
    recording->sampleCount = amplifier->sampleCount();
    recording->durationSeconds = static_cast<double>(recording->sampleCount) / sampleRate;
    recording->channelCount = channelCount;
    recording->channels = parseElectrodeCfg(electrodeCfgPath, channelCount, settings.labels);
    recording->amplifier = amplifier;
    recording->digitalIn = digitalIn;

    if (digitalIn && digitalIn->isOpen()) {
        int auxIndex = 0;
        for (const DigitalChannelDescriptor& descriptor : config.digitalChannels) {
            if (descriptor.path != digitalPath) {
                continue;
            }

            ChannelInfo channel;
            channel.index = descriptor.bitIndex;
            channel.label = descriptor.label;
            channel.electrodeId = descriptor.bitIndex;
            channel.x = static_cast<float>(auxIndex);
            channel.y = 0.0f;
            channel.sourceKind = ChannelInfo::SourceKind::DigitalBit;
            recording->channels.push_back(channel);
            ++auxIndex;
        }
    }

    return recording;
}

QVector<float> extractChannelWindow(
    const RecordingData& recording,
    const ChannelInfo& channel,
    int startSample,
    int endSample,
    TransformMode transformMode,
    int paddingSamples) {
    QVector<float> output;
    const int start = std::clamp(startSample, 0, recording.sampleCount);
    const int end = std::clamp(endSample, start, recording.sampleCount);
    if (end <= start) {
        return output;
    }

    if (isDigitalChannel(channel)) {
        output.fill(0.0f, end - start);
        if (!recording.digitalIn || !recording.digitalIn->isOpen()) {
            return output;
        }
        const int digitalEnd = std::min(end, recording.digitalIn->sampleCount());
        for (int sampleIndex = start; sampleIndex < digitalEnd; ++sampleIndex) {
            const quint16 rawValue = recording.digitalIn->sampleValue(sampleIndex);
            output[sampleIndex - start] = ((rawValue >> channel.index) & 0x1u) ? 1.0f : 0.0f;
        }
        return output;
    }

    if (!recording.amplifier || !recording.amplifier->isOpen()) {
        return output;
    }

    if (paddingSamples < 0) {
        paddingSamples = recommendedPaddingSamples(recording.sampleRate, transformMode);
    }
    paddingSamples = std::max(paddingSamples, 0);

    const int paddedStart = std::max(start - paddingSamples, 0);
    const int paddedEnd = std::min(end + paddingSamples, recording.sampleCount);
    QVector<float> samples(paddedEnd - paddedStart);
    for (int sampleIndex = paddedStart; sampleIndex < paddedEnd; ++sampleIndex) {
        samples[sampleIndex - paddedStart] = recording.amplifier->sampleUv(sampleIndex, channel.index);
    }

    applyTransformInPlace(&samples, recording.sampleRate, transformMode);
    output = samples.mid(start - paddedStart, end - start);
    return output;
}

QVector<float> estimateEventThresholds(
    const RecordingData& recording,
    const QVector<int>& displayChannelIndices,
    TransformMode transformMode,
    int windowSamples,
    int sampleWindows) {
    QVector<float> thresholds(displayChannelIndices.size(), -20.0f);
    if (displayChannelIndices.isEmpty() || recording.sampleCount <= 0) {
        return thresholds;
    }

    windowSamples = std::max(256, std::min(windowSamples, recording.sampleCount));
    sampleWindows = std::max(1, sampleWindows);
    const int maxStart = std::max(recording.sampleCount - windowSamples, 0);

    std::vector<std::vector<float>> sigmaPerChannel(static_cast<std::size_t>(displayChannelIndices.size()));
    for (int displayRow = 0; displayRow < displayChannelIndices.size(); ++displayRow) {
        if (isDigitalChannel(recording.channels[displayChannelIndices[displayRow]])) {
            thresholds[displayRow] = 0.5f;
        }
    }

    for (int windowIndex = 0; windowIndex < sampleWindows; ++windowIndex) {
        const int start = (sampleWindows == 1)
            ? 0
            : static_cast<int>(std::llround((static_cast<double>(windowIndex) * maxStart) / (sampleWindows - 1)));
        const int end = start + windowSamples;

        for (int displayRow = 0; displayRow < displayChannelIndices.size(); ++displayRow) {
            const ChannelInfo& channel = recording.channels[displayChannelIndices[displayRow]];
            if (isDigitalChannel(channel)) {
                continue;
            }

            QVector<float> signal = extractChannelWindow(recording, channel, start, end, transformMode);
            if (signal.isEmpty()) {
                continue;
            }
            std::vector<float> scratch(signal.begin(), signal.end());
            const float median = medianCopy(scratch);
            for (float& value : scratch) {
                value = std::abs(value - median);
            }
            const float mad = medianCopy(scratch);
            sigmaPerChannel[static_cast<std::size_t>(displayRow)].push_back(mad / 0.6745f);
        }
    }

    for (int displayRow = 0; displayRow < displayChannelIndices.size(); ++displayRow) {
        const ChannelInfo& channel = recording.channels[displayChannelIndices[displayRow]];
        if (isDigitalChannel(channel)) {
            continue;
        }
        float sigma = medianCopy(sigmaPerChannel[static_cast<std::size_t>(displayRow)]);
        sigma = std::max(sigma, 2.5f);
        thresholds[displayRow] = -4.0f * sigma;
    }

    return thresholds;
}

HeatmapResult computeHeatmap(
    const RecordingData& recording,
    const QVector<int>& displayChannelIndices,
    TransformMode transformMode) {
    HeatmapResult result;
    if (displayChannelIndices.isEmpty() || recording.sampleCount <= 0) {
        return result;
    }

    result.eventThresholds = estimateEventThresholds(recording, displayChannelIndices, transformMode);

    QVector<int> order(displayChannelIndices.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](int left, int right) {
        const ChannelInfo& a = recording.channels[displayChannelIndices[left]];
        const ChannelInfo& b = recording.channels[displayChannelIndices[right]];
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

    result.rows = displayChannelIndices.size();
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

    QVector<float> xCoords(displayChannelIndices.size());
    QVector<float> yCoords(displayChannelIndices.size());
    for (int displayRow = 0; displayRow < displayChannelIndices.size(); ++displayRow) {
        const ChannelInfo& channel = recording.channels[displayChannelIndices[displayRow]];
        xCoords[displayRow] = channel.x;
        yCoords[displayRow] = channel.y;
    }

    QVector<float> activity(displayChannelIndices.size());
    QVector<float> events(displayChannelIndices.size());
    QVector<float> rms(displayChannelIndices.size());
    QVector<float> ptp(displayChannelIndices.size());
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

        QVector<float> commonAccumulator(length, 0.0f);
        double commonModeSq = 0.0;
        float populationActivity = 0.0f;
        float populationEvents = 0.0f;

        for (int displayRow = 0; displayRow < displayChannelIndices.size(); ++displayRow) {
            const ChannelInfo& channel = recording.channels[displayChannelIndices[displayRow]];
            QVector<float> signal = extractChannelWindow(recording, channel, start, end, transformMode);
            if (signal.size() != length) {
                signal.resize(length);
            }

            if (isDigitalChannel(channel)) {
                float minValue = 1.0f;
                float maxValue = 0.0f;
                double sum = 0.0;
                double sumSq = 0.0;
                int risingEdges = 0;
                float previous = 0.0f;
                bool havePrevious = false;
                for (int sampleIndex = 0; sampleIndex < length; ++sampleIndex) {
                    const float value = signal[sampleIndex] > 0.5f ? 1.0f : 0.0f;
                    sum += value;
                    sumSq += value * value;
                    minValue = std::min(minValue, value);
                    maxValue = std::max(maxValue, value);
                    commonAccumulator[sampleIndex] += value - 0.5f;
                    if (havePrevious && value > 0.5f && previous <= 0.5f) {
                        ++risingEdges;
                    }
                    previous = value;
                    havePrevious = true;
                }

                activity[displayRow] = static_cast<float>(sum / length);
                rms[displayRow] = static_cast<float>(std::sqrt(sumSq / length));
                ptp[displayRow] = maxValue - minValue;
                events[displayRow] = static_cast<float>(risingEdges);
            } else {
                std::vector<float> scratch(signal.begin(), signal.end());
                const float median = medianCopy(scratch);

                double sumAbs = 0.0;
                double sumSq = 0.0;
                float minCentered = std::numeric_limits<float>::infinity();
                float maxCentered = -std::numeric_limits<float>::infinity();
                int crossingCount = 0;
                float previousCentered = 0.0f;
                bool havePrevious = false;

                for (int sampleIndex = 0; sampleIndex < length; ++sampleIndex) {
                    const float raw = signal[sampleIndex];
                    const float centeredValue = raw - median;
                    commonAccumulator[sampleIndex] += centeredValue;
                    sumAbs += std::abs(raw);
                    sumSq += centeredValue * centeredValue;
                    minCentered = std::min(minCentered, centeredValue);
                    maxCentered = std::max(maxCentered, centeredValue);
                    if (havePrevious && centeredValue < result.eventThresholds[displayRow] && previousCentered >= result.eventThresholds[displayRow]) {
                        ++crossingCount;
                    }
                    previousCentered = centeredValue;
                    havePrevious = true;
                }

                activity[displayRow] = static_cast<float>(sumAbs / length);
                rms[displayRow] = static_cast<float>(std::sqrt(sumSq / length));
                ptp[displayRow] = maxCentered - minCentered;
                events[displayRow] = static_cast<float>(crossingCount);
            }

            populationActivity += activity[displayRow];
            populationEvents += events[displayRow];
        }

        for (float sampleMean : commonAccumulator) {
            sampleMean /= std::max(displayChannelIndices.size(), 1);
            commonModeSq += sampleMean * sampleMean;
        }
        populationActivity /= std::max(displayChannelIndices.size(), 1);

        const float baseline = [&]() {
            std::vector<float> activityCopy(activity.begin(), activity.end());
            return medianCopy(activityCopy);
        }();

        double weightSum = 0.0;
        double xCenter = 0.0;
        double yCenter = 0.0;
        for (int displayRow = 0; displayRow < displayChannelIndices.size(); ++displayRow) {
            double weight = std::max(0.0f, activity[displayRow] - baseline);
            if (weight <= 1.0e-6) {
                weight = std::max(0.0f, activity[displayRow]);
            }
            weightSum += weight;
            xCenter += weight * xCoords[displayRow];
            yCenter += weight * yCoords[displayRow];
        }
        if (weightSum > 1.0e-6) {
            xCenters[binIndex] = static_cast<float>(xCenter / weightSum);
            yCenters[binIndex] = static_cast<float>(yCenter / weightSum);
        }

        result.populationActivity[binIndex] = populationActivity;
        result.populationEvents[binIndex] = populationEvents;
        result.commonModeSeries[binIndex] = static_cast<float>(std::sqrt(commonModeSq / length));

        for (int rowIndex = 0; rowIndex < result.rows; ++rowIndex) {
            const int sourceRow = order[rowIndex];
            const int matrixIndex = rowIndex * result.cols + binIndex;
            result.activityMatrix[matrixIndex] = activity[sourceRow];
            result.eventMatrix[matrixIndex] = events[sourceRow];
            result.rmsMatrix[matrixIndex] = rms[sourceRow];
            result.ptpMatrix[matrixIndex] = ptp[sourceRow];
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

QString transformModeName(TransformMode mode) {
    switch (mode) {
    case TransformMode::Raw:
        return QStringLiteral("Raw");
    case TransformMode::Highpass300:
        return QStringLiteral("High-pass 300 Hz");
    case TransformMode::Bandpass300To6000:
        return QStringLiteral("Band-pass 300-6000 Hz");
    case TransformMode::Bandpass500To3000:
        return QStringLiteral("Band-pass 500-3000 Hz");
    case TransformMode::Lowpass250:
        return QStringLiteral("Low-pass 250 Hz");
    case TransformMode::Notch60:
        return QStringLiteral("Notch 60 Hz");
    }
    return QStringLiteral("Raw");
}

}  // namespace spikeviewer
