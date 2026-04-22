#include <QGroupBox>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QMap>
#include <QtCore/QMimeData>
#include <QtCore/QProcess>
#include <QtCore/QRegularExpression>
#include <QtCore/QSet>
#include <QtCore/QSettings>
#include <QtCore/QSignalBlocker>
#include <QtCore/QStandardPaths>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtCore/QUuid>
#include <QtGui/QCloseEvent>
#include <QtGui/QDesktopServices>
#include <QtGui/QDragEnterEvent>
#include <QtGui/QIcon>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtGui/QPixmap>
#include <QtGui/QResizeEvent>
#include <QtMultimedia/QAudioBuffer>
#include <QtMultimedia/QAudioBufferOutput>
#include <QtMultimedia/QAudioOutput>
#include <QtMultimedia/QAudioSink>
#include <QtMultimedia/QMediaMetaData>
#include <QtMultimedia/QMediaPlayer>
#include <QtMultimediaWidgets/QVideoWidget>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QFrame>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSizePolicy>
#include <QtWidgets/QSlider>
#include <QtWidgets/QStyle>
#include <QtWidgets/QStyleOptionSlider>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>

#include <cmath>
#include <optional>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {
constexpr int MAX_ATTEMPTS = 5;

const char *APP_QSS = R"QSS(
QMainWindow, QWidget { background-color: #14181d; color: #e7edf3; font-family: "Segoe UI"; font-size: 13px; }
QFrame { background-color: #1b222a; border: 1px solid #2a3440; border-radius: 10px; }
QLabel { color: #e7edf3; }
QLabel#fieldCaption {
    color: #e7edf3;
    background-color: #18222d;
    border: 1px solid #2a3643;
    border-radius: 10px;
    padding: 0 10px;
}
QLineEdit {
    background-color: #0f141a;
    color: #e7edf3;
    border: 1px solid #344353;
    border-radius: 6px;
    padding: 7px 9px;
    min-height: 22px;
    max-height: 22px;
}
QComboBox {
    background-color: #0f141a;
    color: #e7edf3;
    border: 1px solid #344353;
    border-radius: 6px;
    padding: 6px 10px;
    min-height: 24px;
    max-height: 24px;
}
QComboBox::drop-down {
    border: none;
    width: 22px;
}
QPushButton { background-color: #1f56c7; color: #fff; border: none; border-radius: 8px; padding: 8px 12px; font-weight: 600; }
QPushButton:hover { background-color: #1a4aa9; }
QPushButton:disabled { background-color: #3a4552; color: #95a3b1; }
QSlider::groove:horizontal { height: 10px; background: #2b3642; border-radius: 4px; }
QSlider::handle:horizontal { background: #3d8bfd; border: 2px solid #2f6fd0; width: 18px; margin: -6px 0; border-radius: 9px; }
QProgressBar {
    background: #18212b;
    border: 1px solid #304050;
    border-radius: 8px;
    color: #eef4fb;
    text-align: center;
    min-height: 18px;
    padding: 0 6px;
    font-weight: 600;
}
QProgressBar::chunk {
    border-radius: 7px;
    background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #2f80ed, stop:1 #57a5ff);
}
#previewView { background: #0c1015; border: 1px solid #2f3b49; border-radius: 8px; }
#controlsBar { background: #171e26; border: 1px solid #2b3643; border-radius: 8px; }
#previewBadge {
    background: #1b2430;
    border: 1px solid #334152;
    border-radius: 10px;
}
#previewBadge QLabel {
    color: #d7e0ea;
    font-size: 16px;
    font-weight: 600;
}
#previewBadgeClose {
    background: transparent;
    color: #d7e0ea;
    border: none;
    padding: 2px 4px;
    min-width: 22px;
    max-width: 22px;
    font-size: 16px;
    font-weight: 700;
}
#previewBadgeClose:hover {
    color: #ffffff;
}
)QSS";

struct MediaInfo {
  double duration = 0.0;
  int width = 0;
  int height = 0;
  bool ok = false;
};

// Soft exponential volume curve: 100% = 1x, 150% = 2x, 200% = 4x
inline float sliderToGain(int sliderValue) {
  if (sliderValue <= 0)
    return 0.0f;
  return std::pow(2.0f, (sliderValue - 100) / 50.0f);
}

// ---------------------------------------------------------------------------
// GainAudioSink — replaces QAudioOutput and applies software gain (0.0–2.0+)
// Uses QAudioBufferOutput to intercept raw PCM buffers from QMediaPlayer,
// applies per-sample gain, then writes to QAudioSink.
// ---------------------------------------------------------------------------
class GainAudioSink : public QObject {
  Q_OBJECT
public:
  explicit GainAudioSink(QObject *parent = nullptr) : QObject(parent) {}
  ~GainAudioSink() { detach(); }

  void attachTo(QMediaPlayer *player) {
    detach();
    player_ = player;
    connect(player_, &QObject::destroyed, this,
            [this]() { player_ = nullptr; });
    bufferOutput_ = new QAudioBufferOutput(this);
    player_->setAudioBufferOutput(bufferOutput_);
    connect(bufferOutput_, &QAudioBufferOutput::audioBufferReceived, this,
            &GainAudioSink::onBuffer);
  }

  void detach() {
    if (player_) {
      player_->setAudioBufferOutput(nullptr);
      player_ = nullptr;
    }
    if (bufferOutput_) {
      bufferOutput_->deleteLater();
      bufferOutput_ = nullptr;
    }
    resetSink();
  }

  void setVolume(float vol) { volume_ = vol; }
  float volume() const { return volume_; }
  void setMuted(bool muted) { muted_ = muted; }
  bool muted() const { return muted_; }

private:
  void resetSink() {
    if (sink_) {
      sink_->stop();
      sink_->deleteLater();
      sink_ = nullptr;
      sinkDevice_ = nullptr;
    }
    lastFormat_ = {};
  }

  void onBuffer(const QAudioBuffer &buf) {
    if (muted_ || volume_ < 0.001f)
      return;
    if (!buf.isValid() || buf.byteCount() == 0)
      return;

    if (!sink_ || lastFormat_ != buf.format()) {
      resetSink();
      lastFormat_ = buf.format();
      sink_ = new QAudioSink(lastFormat_, this);
      sink_->setBufferSize(buf.byteCount() * 8);
      sinkDevice_ = sink_->start();
    }
    if (!sinkDevice_ || !sinkDevice_->isOpen())
      return;

    if (std::abs(volume_ - 1.0f) < 0.002f) {
      sinkDevice_->write(
          reinterpret_cast<const char *>(buf.constData<quint8>()),
          buf.byteCount());
      return;
    }
    QByteArray data(reinterpret_cast<const char *>(buf.constData<quint8>()),
                    buf.byteCount());
    applyGain(data, buf.format(), volume_);
    sinkDevice_->write(data.constData(), data.size());
  }

  static float softClip(float x) { return std::tanh(x); }

  static void applyGain(QByteArray &data, const QAudioFormat &fmt, float gain) {
    switch (fmt.sampleFormat()) {
    case QAudioFormat::Float: {
      auto *s = reinterpret_cast<float *>(data.data());
      const int n = data.size() / int(sizeof(float));
      for (int i = 0; i < n; ++i)
        s[i] = softClip(s[i] * gain);
      break;
    }
    case QAudioFormat::Int16: {
      auto *s = reinterpret_cast<int16_t *>(data.data());
      const int n = data.size() / int(sizeof(int16_t));
      for (int i = 0; i < n; ++i) {
        const float v = softClip((s[i] / 32768.0f) * gain);
        s[i] = static_cast<int16_t>(v * 32767.0f);
      }
      break;
    }
    case QAudioFormat::Int32: {
      auto *s = reinterpret_cast<int32_t *>(data.data());
      const int n = data.size() / int(sizeof(int32_t));
      constexpr float sc = 2147483647.0f;
      for (int i = 0; i < n; ++i) {
        const float v = softClip((s[i] / 2147483648.0f) * gain);
        s[i] = static_cast<int32_t>(v * sc);
      }
      break;
    }
    case QAudioFormat::UInt8: {
      auto *s = reinterpret_cast<uint8_t *>(data.data());
      const int n = data.size();
      for (int i = 0; i < n; ++i) {
        const float v = softClip(((s[i] - 128) / 128.0f) * gain);
        s[i] = static_cast<uint8_t>(v * 127.0f + 128.0f);
      }
      break;
    }
    default:
      break;
    }
  }

  QMediaPlayer *player_ = nullptr;
  QAudioBufferOutput *bufferOutput_ = nullptr;
  QAudioSink *sink_ = nullptr;
  QIODevice *sinkDevice_ = nullptr;
  QAudioFormat lastFormat_;
  float volume_ = 1.0f;
  bool muted_ = false;
};

struct AudioTrackInfo {
  int streamIndex = -1;
  QString label;
};

struct Request {
  QString path;
  QString compressMode;
  double targetMb = 0.0;
  MediaInfo mediaInfo;
  QString startMmss;
  QString endMmss;
  int durationSeconds = 0;
  int targetDuration = 1;
  int presetVideoKbps = 0;
  int presetAudioKbps = 0;
  int presetWidthLimit = 0;
  QString presetFfmpegPreset = "medium";
  QVector<int> selectedAudioStreamIndices;
};

QString padTime(const QString &value) {
  const QStringList parts = value.trimmed().split(':');
  bool ok = false;
  if (parts.size() == 2) {
    const int mm = parts[0].toInt(&ok);
    if (!ok)
      return {};
    const int ss = parts[1].toInt(&ok);
    if (!ok || mm < 0 || ss < 0 || ss >= 60)
      return {};
    return QString("%1:%2")
        .arg(mm, 2, 10, QChar('0'))
        .arg(ss, 2, 10, QChar('0'));
  }
  if (parts.size() == 1) {
    const int t = parts[0].toInt(&ok);
    if (!ok || t < 0)
      return {};
    return QString("%1:%2")
        .arg(t / 60, 2, 10, QChar('0'))
        .arg(t % 60, 2, 10, QChar('0'));
  }
  return {};
}

int mmssToSeconds(const QString &mmss) {
  const auto p = mmss.split(':');
  return p.size() == 2 ? p[0].toInt() * 60 + p[1].toInt() : 0;
}

int parseFfmpegProgress(const QString &line) {
  static QRegularExpression rx(R"(time=(\d+):(\d+):(\d+)\.(\d+))");
  auto m = rx.match(line);
  if (!m.hasMatch())
    return -1;
  return m.captured(1).toInt() * 3600 + m.captured(2).toInt() * 60 +
         m.captured(3).toInt();
}

MediaInfo getMediaInfo(const QString &path) {
  QProcess p;
  p.start("ffprobe", {"-v", "error", "-select_streams", "v:0", "-show_entries",
                      "stream=width,height", "-show_entries", "format=duration",
                      "-of", "default=noprint_wrappers=1", path});
  if (!p.waitForFinished(12000) || p.exitCode() != 0)
    return {};

  QMap<QString, QString> map;
  for (const QString &line : QString::fromUtf8(p.readAllStandardOutput())
                                 .split('\n', Qt::SkipEmptyParts)) {
    int i = line.indexOf('=');
    if (i > 0)
      map[line.left(i).trimmed()] = line.mid(i + 1).trimmed();
  }
  MediaInfo info;
  bool okD = false, okW = false, okH = false;
  info.duration = map.value("duration").toDouble(&okD);
  info.width = map.value("width").toInt(&okW);
  info.height = map.value("height").toInt(&okH);
  info.ok = okD;
  if (!okW)
    info.width = 0;
  if (!okH)
    info.height = 0;
  return info;
}

static QString iso639ToName(const QString &code) {
  static const QMap<QString, QString> map = {
      {"eng", "English"},  {"en", "English"},     {"fre", "French"},
      {"fra", "French"},   {"fr", "French"},      {"ger", "German"},
      {"deu", "German"},   {"de", "German"},      {"spa", "Spanish"},
      {"es", "Spanish"},   {"ita", "Italian"},    {"it", "Italian"},
      {"jpn", "Japanese"}, {"ja", "Japanese"},    {"kor", "Korean"},
      {"ko", "Korean"},    {"por", "Portuguese"}, {"pt", "Portuguese"},
      {"rus", "Russian"},  {"ru", "Russian"},     {"chi", "Chinese"},
      {"zho", "Chinese"},  {"zh", "Chinese"},     {"ara", "Arabic"},
      {"ar", "Arabic"},    {"dut", "Dutch"},      {"nld", "Dutch"},
      {"nl", "Dutch"},     {"pol", "Polish"},     {"pl", "Polish"},
      {"tur", "Turkish"},  {"tr", "Turkish"},     {"swe", "Swedish"},
      {"sv", "Swedish"},   {"nor", "Norwegian"},  {"no", "Norwegian"},
      {"dan", "Danish"},   {"da", "Danish"},      {"fin", "Finnish"},
      {"fi", "Finnish"},   {"heb", "Hebrew"},     {"he", "Hebrew"},
      {"und", ""},         {"unk", ""},
  };
  const QString lower = code.toLower();
  return map.value(lower, code);
}

QVector<AudioTrackInfo> getAudioTracks(const QString &path) {
  QProcess p;
  p.start(
      "ffprobe",
      {"-v", "error", "-select_streams", "a", "-show_entries",
       "stream=index,codec_name,channels,bit_rate:stream_tags=language,title",
       "-of", "json", path});
  if (!p.waitForFinished(12000) || p.exitCode() != 0)
    return {};

  const auto doc = QJsonDocument::fromJson(p.readAllStandardOutput());
  if (!doc.isObject())
    return {};
  const auto streams = doc.object().value("streams").toArray();
  QVector<AudioTrackInfo> tracks;
  tracks.reserve(streams.size());
  int audioTrackNumber = 0;
  for (const auto &value : streams) {
    const auto obj = value.toObject();
    AudioTrackInfo track;
    track.streamIndex = obj.value("index").toInt(-1);
    if (track.streamIndex < 0)
      continue;
    ++audioTrackNumber;

    const QString codec = obj.value("codec_name").toString().toUpper();
    const int channels = obj.value("channels").toInt(0);
    const int bitrateKbps = obj.value("bit_rate").toString().toInt() / 1000;
    const auto tags = obj.value("tags").toObject();
    const QString langCode = tags.value("language").toString().trimmed();
    const QString langName = iso639ToName(langCode);
    const QString title = tags.value("title").toString().trimmed();

    // Human-readable channel layout
    QString chStr;
    if (channels == 1)
      chStr = "Mono";
    else if (channels == 2)
      chStr = "Stereo";
    else if (channels == 6)
      chStr = "5.1";
    else if (channels == 8)
      chStr = "7.1";
    else if (channels > 0)
      chStr = QString("%1 ch").arg(channels);

    QStringList parts;
    parts << QString("Track %1").arg(audioTrackNumber);
    if (!langName.isEmpty())
      parts << langName;
    if (!title.isEmpty() && title.compare(langCode, Qt::CaseInsensitive) != 0)
      parts << title;
    if (!codec.isEmpty())
      parts << codec;
    if (!chStr.isEmpty())
      parts << chStr;
    if (bitrateKbps > 0)
      parts << QString("%1 kb/s").arg(bitrateKbps);
    track.label = parts.join("  ·  ");
    tracks.push_back(track);
  }
  return tracks;
}

double parseSizeMb(const QString &value) {
  QString v = value;
  v.replace(',', '.');
  bool ok = false;
  const double mb = v.trimmed().toDouble(&ok);
  return (ok && mb > 0.0) ? mb : -1.0;
}

int even(int v) {
  v = qMax(2, v);
  return v % 2 == 0 ? v : v - 1;
}
int chooseAudioBitrate(int total) {
  if (total >= 2200)
    return 160;
  if (total >= 1200)
    return 128;
  if (total >= 700)
    return 96;
  if (total >= 420)
    return 64;
  return 48;
}

int chooseTargetWidth(int sw, int sh, int vk) {
  if (sw <= 0 || sh <= 0)
    return 0;
  int w = sw;
  if (vk >= 2500)
    w = qMin(sw, 1920);
  else if (vk >= 1500)
    w = qMin(sw, 1280);
  else if (vk >= 900)
    w = qMin(sw, 960);
  else if (vk >= 600)
    w = qMin(sw, 854);
  else if (vk >= 380)
    w = qMin(sw, 640);
  else
    w = qMin(sw, 426);
  return even(w);
}

QString buildScaleFilter(int sw, int sh, int vk) {
  int tw = chooseTargetWidth(sw, sh, vk);
  if (tw <= 0 || tw >= sw)
    return {};
  int th = even(static_cast<int>(tw * (static_cast<double>(sh) / sw)));
  return QString("scale=%1:%2").arg(tw).arg(th);
}

QString buildOutputPath(const QString &inputPath, const QString &startLabel,
                        const QString &endLabel) {
  QFileInfo fi(inputPath);
  const QString base = fi.completeBaseName();
  const QString dir = fi.absolutePath();
  QString out = QDir(dir).filePath(
      QString("%1_%2_%3.mp4").arg(base, startLabel, endLabel));
  int idx = 1;
  while (QFileInfo::exists(out)) {
    out = QDir(dir).filePath(QString("%1_%2_%3_compressed(%4).mp4")
                                 .arg(base, startLabel, endLabel)
                                 .arg(idx++));
  }
  return out;
}

QString formatMs(qint64 ms) {
  int s = qMax<qint64>(0, ms / 1000);
  return QString("%1:%2")
      .arg(s / 60, 2, 10, QChar('0'))
      .arg(s % 60, 2, 10, QChar('0'));
}

bool rmDir(const QString &p) {
  QDir d(p);
  return !d.exists() || d.removeRecursively();
}
} // namespace

class SeekSlider : public QSlider {
  Q_OBJECT
public:
  explicit SeekSlider(Qt::Orientation o, QWidget *parent = nullptr)
      : QSlider(o, parent) {}

  void setTrimMarkers(int startMs, int endMs) {
    startMarkerMs_ = startMs;
    endMarkerMs_ = endMs;
    update();
  }

protected:
  void mousePressEvent(QMouseEvent *e) override {
    if (e->button() == Qt::LeftButton && maximum() > minimum()) {
      int v = QStyle::sliderValueFromPosition(
          minimum(), maximum(), e->position().x(), qMax(1, width()));
      setValue(v);
      emit sliderMoved(v);
    }
    QSlider::mousePressEvent(e);
  }

  void paintEvent(QPaintEvent *event) override {
    QSlider::paintEvent(event);
    if (orientation() != Qt::Horizontal || maximum() <= minimum())
      return;

    auto markerX = [this](int value, const QRect &grooveRect) {
      const double ratio =
          static_cast<double>(qBound(minimum(), value, maximum()) - minimum()) /
          qMax(1, maximum() - minimum());
      return grooveRect.left() +
             static_cast<int>(std::round(ratio * grooveRect.width()));
    };

    QStyleOptionSlider opt;
    initStyleOption(&opt);
    const QRect grooveRect = style()->subControlRect(
        QStyle::CC_Slider, &opt, QStyle::SC_SliderGroove, this);
    if (!grooveRect.isValid())
      return;

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    auto drawMarker = [&](int value, const QColor &color, bool leftSide) {
      if (value < minimum() || value > maximum())
        return;
      const int x = markerX(value, grooveRect);
      const QRect lineRect(x - 1, grooveRect.top() - 8, 3,
                           grooveRect.height() + 16);
      p.fillRect(lineRect, color);

      QPainterPath flag;
      const int tipY = grooveRect.top() - 3;
      if (leftSide) {
        flag.moveTo(x - 1, tipY);
        flag.lineTo(x - 9, tipY - 7);
        flag.lineTo(x - 1, tipY - 14);
      } else {
        flag.moveTo(x + 1, tipY);
        flag.lineTo(x + 9, tipY - 7);
        flag.lineTo(x + 1, tipY - 14);
      }
      flag.closeSubpath();
      p.fillPath(flag, color);
    };

    if (startMarkerMs_ >= 0)
      drawMarker(startMarkerMs_, QColor("#47d18c"), true);
    if (endMarkerMs_ >= 0)
      drawMarker(endMarkerMs_, QColor("#ff7b72"), false);
  }

private:
  int startMarkerMs_ = -1;
  int endMarkerMs_ = -1;
};

// No PreviewWorker needed — multi-track audio is handled by auxiliary
// QMediaPlayer instances playing in parallel with the main player.

class CompressionWorker : public QThread {
  Q_OBJECT
public:
  CompressionWorker(Request req, QString outputPath, QObject *parent = nullptr)
      : QThread(parent), req_(std::move(req)),
        outputPath_(std::move(outputPath)) {}

signals:
  void progress(int);
  void status(const QString &);
  void finishedOk(const QString &);
  void failed(const QString &);

protected:
  void run() override {
    int targetDuration = qMax(1, req_.targetDuration);
    if (req_.compressMode == "source") {
      runCutOnly(targetDuration);
      return;
    }
    if (req_.compressMode == "preset") {
      runWithPreset(targetDuration);
      return;
    }
    runWithTargetSize(targetDuration);
  }

private:
  void appendSelectedAudioMaps(QStringList &cmd,
                               const QVector<int> &streamIndices,
                               const QString &inputPrefix = "0") const {
    cmd << "-map" << inputPrefix + ":v:0?";
    for (int streamIndex : streamIndices)
      cmd << "-map" << QString("%1:%2").arg(inputPrefix).arg(streamIndex);
  }

  QStringList buildPassCmd(const QString &path, int vk, int ak,
                           const QString &preset, int passNo,
                           const QString &passlog, const QString &scale,
                           bool includeAudio) const {
    QStringList cmd{"-y", "-i", path};
    if (!scale.isEmpty())
      cmd << "-vf" << scale;
    cmd << "-map" << "0:v:0?";
    cmd << "-c:v" << "libx264" << "-preset" << preset << "-b:v"
        << QString::number(vk) + "k" << "-maxrate"
        << QString::number(static_cast<int>(std::ceil(vk * 1.15))) + "k"
        << "-bufsize" << QString::number(qMax(vk * 2, 300)) + "k" << "-pass"
        << QString::number(passNo) << "-passlogfile" << passlog << "-movflags"
        << "+faststart";
    if (passNo == 1)
      cmd << "-an" << "-f" << "mp4" << "NUL";
    else if (includeAudio)
      cmd << "-map" << "0:a?" << "-c:a" << "aac" << "-b:a"
          << QString::number(ak) + "k" << outputPath_;
    else
      cmd << "-an" << outputPath_;
    return cmd;
  }

  QStringList buildSinglePassCmd(const QString &path, int vk, int ak,
                                 const QString &preset, const QString &scale,
                                 bool includeAudio) const {
    QStringList cmd{"-y", "-i", path};
    if (!scale.isEmpty())
      cmd << "-vf" << scale;
    cmd << "-map" << "0:v:0?" << "-c:v" << "libx264" << "-preset" << preset
        << "-b:v" << QString::number(vk) + "k" << "-maxrate"
        << QString::number(static_cast<int>(std::ceil(vk * 1.2))) + "k"
        << "-bufsize" << QString::number(qMax(vk * 2, 300)) + "k";
    if (includeAudio)
      cmd << "-map" << "0:a?" << "-c:a" << "aac" << "-b:a"
          << QString::number(ak) + "k";
    else
      cmd << "-an";
    cmd << "-movflags" << "+faststart" << outputPath_;
    return cmd;
  }

  int runStep(const QStringList &cmd, int stepIdx, int totalSteps,
              int targetDuration) {
    QProcess p;
    p.start("ffmpeg", cmd);
    if (!p.waitForStarted())
      return -1;
    QString pending;
    while (p.state() != QProcess::NotRunning) {
      if (!p.waitForReadyRead(200))
        continue;
      pending += QString::fromUtf8(p.readAllStandardError());
      int pos;
      while ((pos = pending.indexOf('\n')) >= 0) {
        QString line = pending.left(pos);
        pending.remove(0, pos + 1);
        int sec = parseFfmpegProgress(line);
        if (sec < 0)
          continue;
        double stepProg =
            qMin(static_cast<double>(sec) / qMax(1, targetDuration), 0.99);
        int pct = static_cast<int>(((stepIdx + stepProg) / totalSteps) * 100.0);
        emit progress(qBound(0, pct, 100));
      }
    }
    p.waitForFinished();
    return p.exitCode();
  }

  void runCutOnly(int targetDuration) {
    emit status("Input mode: cut only");
    QStringList cut{"-y", "-ss", "00:" + req_.startMmss, "-i", req_.path};
    if (req_.durationSeconds > 0)
      cut << "-t" << QString::number(req_.durationSeconds);
    appendSelectedAudioMaps(cut, req_.selectedAudioStreamIndices);
    cut << "-c" << "copy" << outputPath_;
    if (runStep(cut, 0, 1, targetDuration) != 0) {
      emit failed("Error while cutting");
      return;
    }
    if (!QFileInfo::exists(outputPath_)) {
      emit failed("Cut output file was not created");
      return;
    }
    double mb = QFileInfo(outputPath_).size() / (1024.0 * 1024.0);
    emit progress(100);
    emit finishedOk(QString("Done: %1  |  %2 MB")
                        .arg(QFileInfo(outputPath_).fileName(),
                             QString::number(mb, 'f', 2)));
  }

  void runWithPreset(int targetDuration) {
    QTemporaryDir td(QDir::temp().filePath("video_cut_segment_qt_XXXXXX"));
    if (!td.isValid()) {
      emit failed("Unable to create temporary directory");
      return;
    }
    QString cutPath = td.filePath("segment_temp.mp4");
    QStringList cut{"-y", "-ss", "00:" + req_.startMmss, "-i", req_.path};
    if (req_.durationSeconds > 0)
      cut << "-t" << QString::number(req_.durationSeconds);
    appendSelectedAudioMaps(cut, req_.selectedAudioStreamIndices);
    cut << "-c" << "copy" << cutPath;

    emit status("Step 1/2  Cutting segment");
    if (runStep(cut, 0, 2, targetDuration) != 0) {
      emit failed("Error while creating temporary cut");
      return;
    }

    QString scale;
    MediaInfo info = getMediaInfo(cutPath);
    if (info.ok && req_.presetWidthLimit > 0 &&
        info.width > req_.presetWidthLimit) {
      int h = even(
          static_cast<int>(req_.presetWidthLimit *
                           (static_cast<double>(info.height) / info.width)));
      scale = QString("scale=%1:%2").arg(even(req_.presetWidthLimit)).arg(h);
    }

    emit status(
        QString("Step 2/2  Preset compression | video %1 kb/s | audio %2 kb/s")
            .arg(req_.presetVideoKbps)
            .arg(req_.presetAudioKbps));
    if (runStep(buildSinglePassCmd(cutPath, req_.presetVideoKbps,
                                   req_.presetAudioKbps,
                                   req_.presetFfmpegPreset, scale,
                                   !req_.selectedAudioStreamIndices.isEmpty()),
                1, 2, targetDuration) != 0) {
      emit failed("Error during preset compression");
      return;
    }
    if (!QFileInfo::exists(outputPath_)) {
      emit failed("Output file was not created");
      return;
    }
    double mb = QFileInfo(outputPath_).size() / (1024.0 * 1024.0);
    emit progress(100);
    emit finishedOk(QString("Done: %1  |  %2 MB")
                        .arg(QFileInfo(outputPath_).fileName(),
                             QString::number(mb, 'f', 2)));
  }

  void runWithTargetSize(int targetDuration) {
    qint64 targetBytes = static_cast<qint64>(req_.targetMb * 1024.0 * 1024.0);
    QTemporaryDir td(QDir::temp().filePath("video_cut_segment_qt_XXXXXX"));
    if (!td.isValid()) {
      emit failed("Unable to create temporary directory");
      return;
    }
    QString cutPath = td.filePath("segment_temp.mp4");
    QStringList cut{"-y", "-ss", "00:" + req_.startMmss, "-i", req_.path};
    if (req_.durationSeconds > 0)
      cut << "-t" << QString::number(req_.durationSeconds);
    appendSelectedAudioMaps(cut, req_.selectedAudioStreamIndices);
    cut << "-c" << "copy" << cutPath;

    emit status("Step 1/2  Cutting segment");
    if (runStep(cut, 0, 1 + MAX_ATTEMPTS * 2, targetDuration) != 0) {
      emit failed("Error while creating temporary cut");
      return;
    }

    MediaInfo info = getMediaInfo(cutPath);
    if (!info.ok) {
      emit failed("Unable to read temporary segment");
      return;
    }

    int totalKbps = qMax(
        static_cast<int>((targetBytes * 8.0) / targetDuration / 1000.0), 120);
    int audioKbps = chooseAudioBitrate(totalKbps);
    int videoKbps = qMax(totalKbps - audioKbps - 24, 80);

    QTemporaryDir pd(QDir::temp().filePath("video_cut_passlog_qt_XXXXXX"));
    if (!pd.isValid()) {
      emit failed("Unable to create passlog directory");
      return;
    }
    QString passlog = pd.filePath("ffmpeg2pass");
    int totalSteps = 1 + MAX_ATTEMPTS * 2;

    for (int a = 0; a < MAX_ATTEMPTS; ++a) {
      QString scale = buildScaleFilter(info.width, info.height, videoKbps);
      emit status(QString("Step 2/2  Compression  Attempt %1/%2  | video %3 "
                          "kb/s | audio %4 kb/s")
                      .arg(a + 1)
                      .arg(MAX_ATTEMPTS)
                      .arg(videoKbps)
                      .arg(audioKbps));
      int base = 1 + a * 2;
      if (runStep(buildPassCmd(cutPath, videoKbps, audioKbps, "slow", 1,
                               passlog, scale, false),
                  base, totalSteps, targetDuration) != 0) {
        emit failed("ffmpeg error during pass 1");
        return;
      }
      if (runStep(buildPassCmd(cutPath, videoKbps, audioKbps, "slow", 2,
                               passlog, scale,
                               !req_.selectedAudioStreamIndices.isEmpty()),
                  base + 1, totalSteps, targetDuration) != 0) {
        emit failed("ffmpeg error during pass 2");
        return;
      }
      if (!QFileInfo::exists(outputPath_)) {
        emit failed("Output file was not created");
        return;
      }

      qint64 actualBytes = QFileInfo(outputPath_).size();
      double actualMb = actualBytes / (1024.0 * 1024.0);
      if (actualBytes <= targetBytes) {
        emit progress(100);
        emit finishedOk(QString("Done: %1  |  %2 MB")
                            .arg(QFileInfo(outputPath_).fileName(),
                                 QString::number(actualMb, 'f', 2)));
        return;
      }
      double ratio = static_cast<double>(targetBytes) / actualBytes;
      videoKbps = ratio < 0.55
                      ? qMax(static_cast<int>(videoKbps * 0.7), 70)
                      : qMax(static_cast<int>(videoKbps * ratio * 0.96), 70);
      if (actualMb > req_.targetMb * 1.25)
        audioKbps = qMax(48, qMin(audioKbps, 96));
      else if (actualMb > req_.targetMb * 1.1)
        audioKbps = qMax(64, qMin(audioKbps, 128));
    }
    emit failed("Could not reach target size after several attempts");
  }

  Request req_;
  QString outputPath_;
};
class MainWindow : public QMainWindow {
  Q_OBJECT
public:
  MainWindow() : settings_("VideoCutCompress", "VideoCutCompress", this) {
    setAcceptDrops(true);
    setupUi();
    setupPlayer();
    loadUiSettings();
    updateModeUi();
    updateUiState();
    // Intercept drag events from all child widgets
    qApp->installEventFilter(this);
  }
  ~MainWindow() override { cleanupPreviewTemp(); }

protected:
  void resizeEvent(QResizeEvent *event) override {
    QMainWindow::resizeEvent(event);
    if (videoWidget_)
      videoWidget_->updateGeometry();
  }
  void closeEvent(QCloseEvent *event) override {
    cleanupPreviewTemp();
    QMainWindow::closeEvent(event);
  }
  bool eventFilter(QObject *obj, QEvent *event) override {
    // Forward drag/drop from any child widget to this window
    if (obj != this && isAncestorOf(qobject_cast<QWidget *>(obj))) {
      if (event->type() == QEvent::DragEnter) {
        auto *de = static_cast<QDragEnterEvent *>(event);
        if (de->mimeData()->hasUrls()) {
          const auto urls = de->mimeData()->urls();
          if (!urls.isEmpty() && urls.first().isLocalFile()) {
            de->acceptProposedAction();
            return true;
          }
        }
      } else if (event->type() == QEvent::Drop) {
        auto *de = static_cast<QDropEvent *>(event);
        const auto urls = de->mimeData()->urls();
        if (!urls.isEmpty()) {
          const QString path =
              QDir::toNativeSeparators(urls.first().toLocalFile());
          if (!path.isEmpty()) {
            loadVideoFile(path);
            de->acceptProposedAction();
            return true;
          }
        }
      }
    }
    return QMainWindow::eventFilter(obj, event);
  }
  void dragEnterEvent(QDragEnterEvent *event) override {
    if (event->mimeData()->hasUrls()) {
      const auto urls = event->mimeData()->urls();
      if (!urls.isEmpty() && urls.first().isLocalFile())
        event->acceptProposedAction();
    }
  }
  void dropEvent(QDropEvent *event) override {
    const auto urls = event->mimeData()->urls();
    if (urls.isEmpty())
      return;
    const QString path = QDir::toNativeSeparators(urls.first().toLocalFile());
    if (!path.isEmpty())
      loadVideoFile(path);
  }

private:
  void setupUi() {
    setWindowTitle("Video Cut + Compress");
    resize(1100, 750);

    auto *central = new QWidget(this);
    setCentralWidget(central);
    auto *root = new QHBoxLayout(central);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(16);

    // ==================
    // LEFT PANEL (Settings)
    // ==================
    auto *leftPanel = new QWidget(this);
    leftPanel->setMinimumWidth(340);
    leftPanel->setMaximumWidth(400);
    auto *leftLayout = new QVBoxLayout(leftPanel);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(16);

    auto makeLabel = [](const QString &text) {
      auto *lbl = new QLabel(text);
      lbl->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
      lbl->setStyleSheet("color: #aab8c6;");
      return lbl;
    };
    auto wrapField = [](QLabel *lbl, QWidget *widget) {
      auto *lay = new QVBoxLayout();
      lay->setSpacing(4);
      lay->setContentsMargins(0, 0, 0, 0);
      lay->addWidget(lbl);
      lay->addWidget(widget);
      return lay;
    };

    // --- 1. Source ---
    auto *srcGroup = new QGroupBox("1. Source Video", this);
    auto *srcGrpLayout = new QVBoxLayout(srcGroup);
    srcGrpLayout->setSpacing(12);

    auto *fileRow = new QHBoxLayout();
    videoPathEdit_ = new QLineEdit(this);
    // Debounce: button states update immediately, ffprobe waits 600ms of
    // inactivity
    mediaInfoTimer_ = new QTimer(this);
    mediaInfoTimer_->setSingleShot(true);
    connect(mediaInfoTimer_, &QTimer::timeout, this, [this]() {
      refreshMediaLabel();
      refreshAudioTrackList();
    });
    previewRefreshTimer_ = new QTimer(this);
    previewRefreshTimer_->setSingleShot(true);
    connect(previewRefreshTimer_, &QTimer::timeout, this,
            [this]() { rebuildPreviewFromCurrentSettings(); });
    connect(videoPathEdit_, &QLineEdit::textChanged, this, [this]() {
      updateButtonStates();
      mediaInfoTimer_->start(600);
    });
    auto *browse = new QPushButton("Browse", this);
    connect(browse, &QPushButton::clicked, this, &MainWindow::selectVideo);
    fileRow->addWidget(videoPathEdit_, 1);
    fileRow->addWidget(browse);

    srcGrpLayout->addWidget(makeLabel("File path:"));
    srcGrpLayout->addLayout(fileRow);

    audioTrackList_ = new QListWidget(this);
    audioTrackList_->setSelectionMode(QAbstractItemView::NoSelection);
    audioTrackList_->setMinimumHeight(108);
    audioTrackList_->setMaximumHeight(140);
    audioTrackList_->setStyleSheet(
        "QListWidget { background-color: #0f141a; border: 1px solid #344353; "
        "border-radius: 6px; padding: 4px; }"
        "QListWidget::item { padding: 3px 4px; }");

    auto *audioButtonsRow = new QHBoxLayout();
    audioButtonsRow->setContentsMargins(0, 0, 0, 0);
    audioButtonsRow->setSpacing(8);
    selectAllAudioButton_ = new QPushButton("Select all", this);
    deselectAllAudioButton_ = new QPushButton("Select none", this);
    connect(selectAllAudioButton_, &QPushButton::clicked, this,
            [this]() { setAllAudioTracksChecked(Qt::Checked); });
    connect(deselectAllAudioButton_, &QPushButton::clicked, this,
            [this]() { setAllAudioTracksChecked(Qt::Unchecked); });
    audioButtonsRow->addWidget(selectAllAudioButton_);
    audioButtonsRow->addWidget(deselectAllAudioButton_);
    audioButtonsRow->addStretch(1);

    srcGrpLayout->addWidget(makeLabel("Audio tracks to keep:"));
    srcGrpLayout->addWidget(audioTrackList_);
    srcGrpLayout->addLayout(audioButtonsRow);
    connect(audioTrackList_, &QListWidget::itemChanged, this,
            [this](QListWidgetItem *) { handleAudioSelectionChanged(); });
    leftLayout->addWidget(srcGroup);

    // --- 2. Target Settings ---
    auto *modeGroup = new QGroupBox("2. Target Settings", this);
    modeSectionWrap_ = modeGroup;
    auto *modeGrpLayout = new QVBoxLayout(modeGroup);
    modeGrpLayout->setSpacing(12);

    modeCombo_ = new QComboBox(this);
    modeCombo_->addItem("Max size", "target_size");
    modeCombo_->addItem("Preset (editable)", "preset");
    modeCombo_->addItem("Input (cut only)", "source");
    connect(modeCombo_, &QComboBox::currentIndexChanged, this,
            &MainWindow::updateModeUi);
    connect(modeCombo_, &QComboBox::currentIndexChanged, this,
            &MainWindow::updateUiState);
    modeGrpLayout->addLayout(
        wrapField(makeLabel("Compression Mode:"), modeCombo_));

    sizeRowWrap_ = new QWidget(this);
    auto *sizeRow = new QVBoxLayout(sizeRowWrap_);
    sizeRow->setContentsMargins(0, 0, 0, 0);
    sizeRow->setSpacing(4);
    maxSizeLabel_ = makeLabel("Max size (MB):");
    maxSizeEdit_ = new QLineEdit("10", this);
    connect(maxSizeEdit_, &QLineEdit::textChanged, this,
            &MainWindow::updateUiState);
    sizeRow->addWidget(maxSizeLabel_);
    sizeRow->addWidget(maxSizeEdit_);
    modeGrpLayout->addWidget(sizeRowWrap_);

    presetWrap_ = new QWidget(this);
    auto *presetGrid = new QGridLayout(presetWrap_);
    presetGrid->setContentsMargins(0, 0, 0, 0);
    presetGrid->setHorizontalSpacing(8);
    presetGrid->setVerticalSpacing(12);

    presetCombo_ = new QComboBox(this);
    presetCombo_->addItems(
        {"Web 720p", "Mobile 480p", "High quality", "Custom"});
    connect(presetCombo_, &QComboBox::currentIndexChanged, this,
            &MainWindow::applyPresetDefaults);
    presetVideoKbpsEdit_ = new QLineEdit("1800", this);
    presetAudioKbpsEdit_ = new QLineEdit("128", this);
    presetWidthLimitEdit_ = new QLineEdit("1280", this);
    presetFfmpegCombo_ = new QComboBox(this);
    presetFfmpegCombo_->addItems({"fast", "medium", "slow"});

    presetGrid->addLayout(wrapField(makeLabel("Preset:"), presetCombo_), 0, 0,
                          1, 2);
    presetGrid->addLayout(
        wrapField(makeLabel("Video kb/s:"), presetVideoKbpsEdit_), 1, 0);
    presetGrid->addLayout(
        wrapField(makeLabel("Audio kb/s:"), presetAudioKbpsEdit_), 1, 1);
    presetGrid->addLayout(
        wrapField(makeLabel("Max width:"), presetWidthLimitEdit_), 2, 0);
    presetGrid->addLayout(
        wrapField(makeLabel("Encoder speed:"), presetFfmpegCombo_), 2, 1);
    modeGrpLayout->addWidget(presetWrap_);

    infoLabel_ = new QLabel(this);
    infoLabel_->setStyleSheet("color:#9cb0c4; font-size: 11px;");
    infoLabel_->setWordWrap(true);
    infoLabel_->setVisible(false);
    modeGrpLayout->addWidget(infoLabel_);

    leftLayout->addWidget(modeGroup);
    leftLayout->addStretch(1);

    // --- 3. Export ---
    auto *actionGroup = new QGroupBox("3. Export", this);
    auto *actionLayout = new QVBoxLayout(actionGroup);
    actionLayout->setSpacing(8);

    runButton_ = new QPushButton("Cut and Compress", this);
    runButton_->setStyleSheet("font-weight: bold; font-size: 13px; "
                              "background-color: #2F80ED; color: white;");
    connect(runButton_, &QPushButton::clicked, this,
            &MainWindow::startCutAndCompress);

    previewButton_ = new QPushButton("Enable Preview Mode", this);
    connect(previewButton_, &QPushButton::clicked, this,
            &MainWindow::startPreview);

    actionLayout->addWidget(runButton_);
    actionLayout->addWidget(previewButton_);
    leftLayout->addWidget(actionGroup);

    // ==================
    // RIGHT PANEL (Video & Tools)
    // ==================
    auto *rightPanel = new QWidget(this);
    auto *rightLayout = new QVBoxLayout(rightPanel);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(12);

    previewBadgeWrap_ = new QFrame(this);
    previewBadgeWrap_->setObjectName("previewBadge");
    previewBadgeWrap_->setVisible(false);
    auto *previewBadgeLayout = new QHBoxLayout(previewBadgeWrap_);
    previewBadgeLayout->setContentsMargins(14, 8, 10, 8);
    previewBadgeLayout->setSpacing(8);
    previewIndicatorLabel_ =
        new QLabel("Preview Mode Active", previewBadgeWrap_);
    previewBadgeCloseButton_ = new QPushButton("×", previewBadgeWrap_);
    previewBadgeCloseButton_->setObjectName("previewBadgeClose");
    previewBadgeCloseButton_->setCursor(Qt::PointingHandCursor);
    previewBadgeCloseButton_->setToolTip("Disable Preview Mode");
    connect(previewBadgeCloseButton_, &QPushButton::clicked, this,
            &MainWindow::startPreview);
    previewBadgeLayout->addWidget(previewIndicatorLabel_);
    previewBadgeLayout->addWidget(previewBadgeCloseButton_);
    rightLayout->addWidget(previewBadgeWrap_, 0, Qt::AlignHCenter);

    videoWidget_ = new QVideoWidget(this);
    videoWidget_->setObjectName("previewView");
    videoWidget_->setMinimumHeight(200);
    videoWidget_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    videoWidget_->setFocusPolicy(Qt::NoFocus);

    QFrame *videoFrame = new QFrame(this);
    videoFrame->setObjectName("videoFrame");
    videoFrame->setStyleSheet("QFrame#videoFrame { background: #111; border: "
                              "1px solid #333; border-radius: 6px; }");
    auto *vLayout = new QVBoxLayout(videoFrame);
    vLayout->setContentsMargins(0, 0, 0, 0);
    vLayout->addWidget(videoWidget_);
    rightLayout->addWidget(videoFrame, 1);

    auto *controlsGrp = new QGroupBox(this);
    auto *controlsLayout = new QVBoxLayout(controlsGrp);
    controlsLayout->setSpacing(12);

    positionSlider_ = new SeekSlider(Qt::Horizontal, this);
    positionSlider_->setRange(0, 0);
    positionSlider_->setSingleStep(1000);
    positionSlider_->setPageStep(5000);
    connect(positionSlider_, &QSlider::sliderPressed, this,
            &MainWindow::onSliderPressed);
    connect(positionSlider_, &QSlider::sliderReleased, this,
            &MainWindow::onSliderReleased);
    connect(positionSlider_, &QSlider::sliderMoved, this,
            &MainWindow::onSliderMoved);
    timeLabel_ = new QLabel("00:00 / 00:00", this);
    timeLabel_->setMinimumWidth(80);
    timeLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    auto *tlLayout = new QHBoxLayout();
    tlLayout->addWidget(positionSlider_, 1);
    tlLayout->addWidget(timeLabel_);
    controlsLayout->addLayout(tlLayout);

    auto *pbRow = new QHBoxLayout();
    pbRow->setSpacing(6);

    playPauseButton_ = new QPushButton("Play", this);
    backButton_ = new QPushButton("-", this);
    backButton_->setToolTip("- 5s");
    forwardButton_ = new QPushButton("+", this);
    forwardButton_->setToolTip("+ 5s");
    stopButton_ = new QPushButton("Stop", this);

    connect(playPauseButton_, &QPushButton::clicked, this,
            &MainWindow::togglePlayPause);
    connect(stopButton_, &QPushButton::clicked, this,
            &MainWindow::stopPlayback);
    connect(backButton_, &QPushButton::clicked, this,
            [this]() { seekRelative(-5000); });
    connect(forwardButton_, &QPushButton::clicked, this,
            [this]() { seekRelative(5000); });

    pbRow->addWidget(backButton_);
    pbRow->addWidget(stopButton_);
    pbRow->addWidget(playPauseButton_);
    pbRow->addWidget(forwardButton_);
    pbRow->addSpacing(16);

    volumeButton_ = new QPushButton(this);
    volumeButton_->setFixedSize(30, 30);
    volumeButton_->setText("");
    volumeButton_->setStyleSheet("padding: 0;");
    volumeButton_->setToolTip("Volume");
    pbRow->addWidget(volumeButton_);

    volumePopup_ = new QWidget(this, Qt::Popup);
    volumePopup_->setObjectName("volumePopup");
    volumePopup_->setStyleSheet(
        "QWidget#volumePopup { background: #1a222c; border: 1px solid #333; "
        "border-radius: 4px; }");
    auto *vPopLayout = new QVBoxLayout(volumePopup_);
    vPopLayout->setContentsMargins(8, 12, 8, 8);
    vPopLayout->setSpacing(8);

    volumeSliderPopup_ = new QSlider(Qt::Vertical, volumePopup_);
    volumeSliderPopup_->setRange(0, 200);
    volumeSliderPopup_->setValue(100);
    volumeSliderPopup_->setMinimumHeight(100);
    volumeLabelPopup_ = new QLabel("100%", volumePopup_);
    volumeLabelPopup_->setAlignment(Qt::AlignCenter);
    volumeLabelPopup_->setStyleSheet("color: #aab8c6; font-size: 11px;");

    vPopLayout->addWidget(volumeSliderPopup_, 0, Qt::AlignHCenter);
    vPopLayout->addWidget(volumeLabelPopup_, 0, Qt::AlignHCenter);

    connect(volumeButton_, &QPushButton::clicked, this, [this]() {
      QPoint pos = volumeButton_->mapToGlobal(QPoint(0, 0));
      pos.ry() -= volumePopup_->sizeHint().height() + 5;
      pos.rx() +=
          (volumeButton_->width() - volumePopup_->sizeHint().width()) / 2;
      volumePopup_->move(pos);
      if (volumePopup_->isHidden())
        volumePopup_->show();
      else
        volumePopup_->hide();
    });

    // old pbRow widget was here
    connect(volumeSliderPopup_, &QSlider::valueChanged, this, [this](int v) {
      globalVolume_ = sliderToGain(v);
      syncPreviewAudioSelection();
      volumeLabelPopup_->setText(QString("%1%").arg(v));
      if (volumeButton_)
        volumeButton_->setIcon(v == 0 ? mutedIcon_ : volumeIcon_);
    });

    // Spaced earlier

    startEdit_ = new QLineEdit(this);
    endEdit_ = new QLineEdit(this);
    startEdit_->setPlaceholderText("MM:SS");
    endEdit_->setPlaceholderText("MM:SS");
    startEdit_->setMaximumWidth(60);
    endEdit_->setMaximumWidth(60);
    connect(startEdit_, &QLineEdit::textChanged, this,
            [this](const QString &) { onTrimInputsChanged(); });
    connect(endEdit_, &QLineEdit::textChanged, this,
            [this](const QString &) { onTrimInputsChanged(); });

    markStartButton_ = new QPushButton("▶ Mark In", this);
    markStartButton_->setStyleSheet(
        "QPushButton { background-color: #1a3d1a; color: #6fcf6f;"
        " border: 1px solid #2e6b2e; border-radius: 6px;"
        " font-size: 11px; font-weight: 600; padding: 0 8px; }"
        "QPushButton:pressed { background-color: #0f2410;"
        " border-color: #4caf50; color: #a5f0a5; }");
    connect(markStartButton_, &QPushButton::clicked, this,
            &MainWindow::markStartFromPlayer);

    markEndButton_ = new QPushButton("Mark Out ■", this);
    markEndButton_->setStyleSheet(
        "QPushButton { background-color: #3d1a1a; color: #f07070;"
        " border: 1px solid #6b2e2e; border-radius: 6px;"
        " font-size: 11px; font-weight: 600; padding: 0 8px; }"
        "QPushButton:pressed { background-color: #240f0f;"
        " border-color: #f44336; color: #ffaaaa; }");
    connect(markEndButton_, &QPushButton::clicked, this,
            &MainWindow::markEndFromPlayer);

    pbRow->addWidget(new QLabel("Start:"));
    pbRow->addWidget(startEdit_);
    pbRow->addWidget(markStartButton_);
    pbRow->addSpacing(12);
    pbRow->addWidget(new QLabel("End:"));
    pbRow->addWidget(endEdit_);
    pbRow->addWidget(markEndButton_);

    controlsLayout->addLayout(pbRow);
    rightLayout->addWidget(controlsGrp);

    auto *footerLayout = new QHBoxLayout();
    footerLayout->setContentsMargins(0, 0, 0, 0);
    statusLabel_ = new QLabel("Status: Idle", this);
    statusLabel_->setStyleSheet("color:#aab8c6;");
    openOutputButton_ = new QPushButton("Open Output Folder", this);
    openOutputButton_->setVisible(false);
    connect(openOutputButton_, &QPushButton::clicked, this,
            &MainWindow::openLastOutputFolder);
    progressBar_ = new QProgressBar(this);
    progressBar_->setFormat("%p%");
    progressBar_->setRange(0, 100);
    progressBar_->setVisible(false);
    progressBar_->setFixedHeight(20);
    progressBar_->setMinimumWidth(320);
    progressBar_->setMaximumWidth(420);
    footerLayout->addWidget(statusLabel_, 1);
    footerLayout->addWidget(openOutputButton_);
    footerLayout->addWidget(progressBar_);
    rightLayout->addLayout(footerLayout);

    // QVideoWidget may use a native surface on Windows; keep controls above it.
    videoFrame->lower();
    controlsGrp->raise();
    statusLabel_->raise();
    progressBar_->raise();

    root->addWidget(leftPanel);
    root->addWidget(rightPanel, 1);

    connect(maxSizeEdit_, &QLineEdit::textChanged, this,
            [this](const QString &) { saveUiSettings(); });
    connect(
        modeCombo_,
        static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
        this, [this](int) { saveUiSettings(); });
    connect(
        presetCombo_,
        static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
        this, [this](int) { saveUiSettings(); });
    connect(presetVideoKbpsEdit_, &QLineEdit::textChanged, this,
            [this](const QString &) { saveUiSettings(); });
    connect(presetAudioKbpsEdit_, &QLineEdit::textChanged, this,
            [this](const QString &) { saveUiSettings(); });
    connect(presetWidthLimitEdit_, &QLineEdit::textChanged, this,
            [this](const QString &) { saveUiSettings(); });
    connect(
        presetFfmpegCombo_,
        static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
        this, [this](int) { saveUiSettings(); });

    for (auto *lineEdit : findChildren<QLineEdit *>()) {
      lineEdit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
      lineEdit->setFixedHeight(30);
    }
    for (auto *combo : findChildren<QComboBox *>()) {
      combo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
      combo->setFixedHeight(30);
    }
    for (auto *button : findChildren<QPushButton *>()) {
      button->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
      button->setFixedHeight(30);
    }
    runButton_->setFixedHeight(40);
    runButton_->setCursor(Qt::PointingHandCursor);

    applyIcons();
    updateModeUi();
    updateTrimMarkers();
    setPlayerControlsEnabled(false);
  }

  void setupPlayer() {
    mainGainSink_ = new GainAudioSink(this);
    player_ = new QMediaPlayer(this);
    mainGainSink_->attachTo(player_);
    auto *dummyAudioOutput = new QAudioOutput(this);
    dummyAudioOutput->setVolume(0.0f);
    player_->setAudioOutput(dummyAudioOutput);
    player_->setVideoOutput(videoWidget_);
    connect(player_, &QMediaPlayer::positionChanged, this,
            &MainWindow::onPositionChanged);
    connect(player_, &QMediaPlayer::positionChanged, this,
            [this](qint64 pos) { syncAuxPosition(pos); });
    connect(player_, &QMediaPlayer::durationChanged, this,
            &MainWindow::onDurationChanged);
    connect(player_, &QMediaPlayer::playbackStateChanged, this,
            &MainWindow::onPlaybackStateChanged);
    connect(player_, &QMediaPlayer::playbackStateChanged, this,
            [this](QMediaPlayer::PlaybackState) { syncAuxPlaybackState(); });
    connect(player_, &QMediaPlayer::tracksChanged, this,
            [this]() { syncPreviewAudioSelection(); });
    connect(player_, &QMediaPlayer::mediaStatusChanged, this,
            [this](QMediaPlayer::MediaStatus status) {
              if (status == QMediaPlayer::LoadedMedia ||
                  status == QMediaPlayer::BufferedMedia) {
                syncPreviewAudioSelection();
              }
            });
  }

  void applyIcons() {
    // All icons are drawn flat in the same #d6e2f0 style for visual consistency
    auto draw = [](auto fn) -> QIcon {
      QPixmap pm(18, 18);
      pm.fill(Qt::transparent);
      QPainter p(&pm);
      p.setRenderHint(QPainter::Antialiasing, true);
      p.setPen(Qt::NoPen);
      p.setBrush(QColor("#d6e2f0"));
      fn(p);
      p.end();
      return QIcon(pm);
    };

    // Play ▶
    playIcon_ = draw([](QPainter &p) {
      QPainterPath path;
      path.moveTo(4.5, 2.5);
      path.lineTo(4.5, 15.5);
      path.lineTo(14.5, 9.0);
      path.closeSubpath();
      p.drawPath(path);
    });

    // Pause ⏸
    pauseIcon_ = draw([](QPainter &p) {
      p.drawRoundedRect(QRectF(3.5, 3.0, 3.5, 12.0), 1.0, 1.0);
      p.drawRoundedRect(QRectF(11.0, 3.0, 3.5, 12.0), 1.0, 1.0);
    });

    // Stop ⏹
    stopButton_->setIcon(draw([](QPainter &p) {
      p.drawRoundedRect(QRectF(3.0, 3.0, 12.0, 12.0), 2.0, 2.0);
    }));

    // Back ◀◀
    backButton_->setIcon(draw([](QPainter &p) {
      QPainterPath p1, p2;
      p1.moveTo(9.0, 9.0);
      p1.lineTo(14.0, 3.5);
      p1.lineTo(14.0, 14.5);
      p1.closeSubpath();
      p2.moveTo(4.0, 9.0);
      p2.lineTo(9.0, 3.5);
      p2.lineTo(9.0, 14.5);
      p2.closeSubpath();
      p.drawPath(p1);
      p.drawPath(p2);
    }));

    // Forward ▶▶
    forwardButton_->setIcon(draw([](QPainter &p) {
      QPainterPath p1, p2;
      p1.moveTo(4.0, 3.5);
      p1.lineTo(4.0, 14.5);
      p1.lineTo(9.0, 9.0);
      p1.closeSubpath();
      p2.moveTo(9.0, 3.5);
      p2.lineTo(9.0, 14.5);
      p2.lineTo(14.0, 9.0);
      p2.closeSubpath();
      p.drawPath(p1);
      p.drawPath(p2);
    }));

    // Volume (speaker + arc, or speaker + X when muted)
    auto makeVolumeIcon = [&draw](bool muted) {
      return draw([muted](QPainter &p) {
        QPainterPath speaker;
        speaker.moveTo(3, 7);
        speaker.lineTo(6.5, 7);
        speaker.lineTo(10.5, 4);
        speaker.lineTo(10.5, 14);
        speaker.lineTo(6.5, 11);
        speaker.lineTo(3, 11);
        speaker.closeSubpath();
        p.drawPath(speaker);

        QPen pen(QColor("#d6e2f0"));
        pen.setWidthF(1.6);
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);

        if (muted) {
          p.drawLine(QPointF(12.5, 7.0), QPointF(15.5, 10.0));
          p.drawLine(QPointF(15.5, 7.0), QPointF(12.5, 10.0));
        } else {
          p.drawArc(QRectF(11.5, 6.0, 4.5, 6.0), -45 * 16, 90 * 16);
        }
      });
    };

    volumeIcon_ = makeVolumeIcon(false);
    mutedIcon_ = makeVolumeIcon(true);
    playPauseButton_->setIcon(playIcon_);
    if (volumeButton_) {
      volumeButton_->setIcon(volumeIcon_);
      volumeButton_->setIconSize(QSize(18, 18));
    }
  }

  void setPlayerControlsEnabled(bool e) {
    playPauseButton_->setEnabled(e);
    stopButton_->setEnabled(e);
    backButton_->setEnabled(e);
    forwardButton_->setEnabled(e);
    positionSlider_->setEnabled(e);
  }

  void applyPresetDefaults() {
    QString p = presetCombo_->currentText();
    if (p == "Web 720p") {
      presetVideoKbpsEdit_->setText("1800");
      presetAudioKbpsEdit_->setText("128");
      presetWidthLimitEdit_->setText("1280");
      presetFfmpegCombo_->setCurrentText("medium");
    } else if (p == "Mobile 480p") {
      presetVideoKbpsEdit_->setText("900");
      presetAudioKbpsEdit_->setText("96");
      presetWidthLimitEdit_->setText("854");
      presetFfmpegCombo_->setCurrentText("fast");
    } else if (p == "High quality") {
      presetVideoKbpsEdit_->setText("3200");
      presetAudioKbpsEdit_->setText("160");
      presetWidthLimitEdit_->setText("1920");
      presetFfmpegCombo_->setCurrentText("slow");
    }
    saveUiSettings();
  }

  void saveUiSettings() {
    settings_.setValue("ui/mode", modeCombo_->currentData().toString());
    settings_.setValue("ui/max_size_mb", maxSizeEdit_->text());
    settings_.setValue("ui/preset_name", presetCombo_->currentText());
    settings_.setValue("ui/preset_video_kbps", presetVideoKbpsEdit_->text());
    settings_.setValue("ui/preset_audio_kbps", presetAudioKbpsEdit_->text());
    settings_.setValue("ui/preset_width_limit", presetWidthLimitEdit_->text());
    settings_.setValue("ui/preset_ffmpeg_speed",
                       presetFfmpegCombo_->currentText());
    settings_.setValue("ui/volume", volumeSliderPopup_->value());
    settings_.setValue("ui/preview_mode", previewModeEnabled_);
  }

  void loadUiSettings() {
    const QString mode = settings_.value("ui/mode", "target_size").toString();
    const QString maxSize = settings_.value("ui/max_size_mb", "10").toString();
    const QString presetName =
        settings_.value("ui/preset_name", "Web 720p").toString();
    const QString presetVideo =
        settings_.value("ui/preset_video_kbps", "1800").toString();
    const QString presetAudio =
        settings_.value("ui/preset_audio_kbps", "128").toString();
    const QString presetWidth =
        settings_.value("ui/preset_width_limit", "1280").toString();
    const QString presetSpeed =
        settings_.value("ui/preset_ffmpeg_speed", "medium").toString();
    const int volume = settings_.value("ui/volume", 100).toInt();
    previewModeEnabled_ = settings_.value("ui/preview_mode", false).toBool();

    maxSizeEdit_->setText(maxSize);
    volumeSliderPopup_->setValue(qBound(0, volume, 200));

    const int modeIndex = modeCombo_->findData(mode);
    if (modeIndex >= 0)
      modeCombo_->setCurrentIndex(modeIndex);

    const int presetIndex = presetCombo_->findText(presetName);
    if (presetIndex >= 0)
      presetCombo_->setCurrentIndex(presetIndex);
    presetVideoKbpsEdit_->setText(presetVideo);
    presetAudioKbpsEdit_->setText(presetAudio);
    presetWidthLimitEdit_->setText(presetWidth);

    const int speedIndex = presetFfmpegCombo_->findText(presetSpeed);
    if (speedIndex >= 0)
      presetFfmpegCombo_->setCurrentIndex(speedIndex);
    updatePreviewButtonText();
  }

  qint64 effectivePreviewStart() const {
    return previewModeEnabled_ ? previewStartMs_ : 0;
  }

  qint64 effectivePreviewEnd() const {
    if (!player_)
      return 0;
    const qint64 playerDuration = qMax<qint64>(0, player_->duration());
    if (!previewModeEnabled_)
      return playerDuration;
    if (previewEndMs_ < 0)
      return playerDuration;
    return qBound(previewStartMs_, previewEndMs_, playerDuration);
  }

  qint64 effectiveTimelineDuration() const {
    const qint64 end = effectivePreviewEnd();
    const qint64 start = effectivePreviewStart();
    return qMax<qint64>(0, end - start);
  }

  qint64 displayPositionFromAbsolute(qint64 absPos) const {
    if (!previewModeEnabled_)
      return absPos;
    return qBound<qint64>(0, absPos - effectivePreviewStart(),
                          effectiveTimelineDuration());
  }

  qint64 absolutePositionFromDisplay(qint64 displayPos) const {
    if (!previewModeEnabled_)
      return displayPos;
    return effectivePreviewStart() +
           qBound<qint64>(0, displayPos, effectiveTimelineDuration());
  }

  void refreshPlaybackTimeline() {
    if (!positionSlider_ || !player_)
      return;
    const qint64 duration = previewModeEnabled_
                                ? effectiveTimelineDuration()
                                : qMax<qint64>(player_->duration(), 0);
    positionSlider_->setRange(0, static_cast<int>(duration));
    const qint64 displayPos = displayPositionFromAbsolute(player_->position());
    if (!sliderDragging_)
      positionSlider_->setValue(static_cast<int>(displayPos));
    updateTimeLabel(displayPos, duration);
    updateTrimMarkers();
    updatePreviewButtonText();
  }

  void updatePreviewButtonText() {
    if (!previewButton_)
      return;
    previewButton_->setText(previewModeEnabled_ ? "Disable Preview Mode"
                                                : "Enable Preview Mode");
    if (previewBadgeWrap_)
      previewBadgeWrap_->setVisible(previewModeEnabled_);
  }

  void updateModeUi() {
    QString m = modeCombo_->currentData().toString();
    bool useSize = m == "target_size", usePreset = m == "preset";
    sizeRowWrap_->setVisible(useSize);
    presetWrap_->setVisible(usePreset);
  }

  MediaInfo probeMediaInfo(const QString &path) {
    if (path == cachedMediaPath_)
      return cachedMediaInfo_;
    cachedMediaPath_ = path;
    cachedMediaInfo_ = getMediaInfo(path);
    return cachedMediaInfo_;
  }

  QVector<AudioTrackInfo> probeAudioTracks(const QString &path) {
    if (path == cachedAudioPath_)
      return cachedAudioTracks_;
    cachedAudioPath_ = path;
    cachedAudioTracks_ = getAudioTracks(path);
    return cachedAudioTracks_;
  }

  QVector<int> checkedAudioStreamIndices() const {
    QVector<int> streamIndices;
    if (!audioTrackList_)
      return streamIndices;
    for (int i = 0; i < audioTrackList_->count(); ++i) {
      auto *item = audioTrackList_->item(i);
      if (!item)
        continue;
      auto *w = audioTrackList_->itemWidget(item);
      auto *cb = w ? w->findChild<QCheckBox *>("trackCb") : nullptr;
      if (cb && cb->isChecked())
        streamIndices.push_back(item->data(Qt::UserRole).toInt());
    }
    return streamIndices;
  }

  float getTrackVolumeAtIndex(int listIndex) const {
    if (!audioTrackList_ || listIndex < 0 ||
        listIndex >= audioTrackList_->count())
      return 1.0f;
    auto *item = audioTrackList_->item(listIndex);
    if (!item)
      return 1.0f;
    auto *w = audioTrackList_->itemWidget(item);
    auto *sl = w ? w->findChild<QSlider *>("trackVol") : nullptr;
    return sl ? sliderToGain(sl->value()) : 1.0f;
  }

  int audioTrackListIndexForStreamIndex(int streamIndex) const {
    for (int i = 0; i < cachedAudioTracks_.size(); ++i) {
      if (cachedAudioTracks_.at(i).streamIndex == streamIndex)
        return i;
    }
    return -1;
  }

  bool previewNeedsGeneratedMedia(const Request &req) const {
    return req.selectedAudioStreamIndices.size() > 1;
  }

  void setAllAudioTracksChecked(Qt::CheckState state) {
    if (!audioTrackList_)
      return;
    for (int i = 0; i < audioTrackList_->count(); ++i) {
      auto *item = audioTrackList_->item(i);
      if (!item)
        continue;
      auto *w = audioTrackList_->itemWidget(item);
      auto *cb = w ? w->findChild<QCheckBox *>("trackCb") : nullptr;
      if (cb) {
        QSignalBlocker blocker(cb);
        cb->setCheckState(state);
      }
    }
    handleAudioSelectionChanged();
  }

  void refreshAudioTrackList() {
    if (!audioTrackList_)
      return;
    const QString path = videoPathEdit_->text().trimmed();
    if (path.isEmpty() || !QFileInfo::exists(path)) {
      audioTrackList_->clear();
      audioTrackList_->setEnabled(false);
      selectAllAudioButton_->setEnabled(false);
      deselectAllAudioButton_->setEnabled(false);
      return;
    }

    QSet<int> checkedTracks;
    QMap<int, int> savedVolumes; // streamIndex -> volume 0-100
    bool hadWidgetItems = false;
    for (int i = 0; i < audioTrackList_->count(); ++i) {
      auto *item = audioTrackList_->item(i);
      if (!item)
        continue;
      auto *w = audioTrackList_->itemWidget(item);
      if (!w)
        continue;
      hadWidgetItems = true;
      const int streamIdx = item->data(Qt::UserRole).toInt();
      auto *cb = w->findChild<QCheckBox *>("trackCb");
      if (cb && cb->isChecked())
        checkedTracks.insert(streamIdx);
      auto *sl = w->findChild<QSlider *>("trackVol");
      if (sl)
        savedVolumes[streamIdx] = sl->value();
    }

    const auto tracks = probeAudioTracks(path);
    audioTrackList_->clear();
    if (tracks.isEmpty()) {
      auto *item = new QListWidgetItem("No audio track detected");
      item->setFlags(Qt::NoItemFlags);
      audioTrackList_->addItem(item);
      audioTrackList_->setEnabled(false);
      selectAllAudioButton_->setEnabled(false);
      deselectAllAudioButton_->setEnabled(false);
      return;
    }

    for (const auto &track : tracks) {
      auto *item = new QListWidgetItem(audioTrackList_);
      item->setData(Qt::UserRole, track.streamIndex);
      item->setFlags(item->flags() & ~Qt::ItemIsUserCheckable);
      item->setSizeHint(QSize(-1, 30));

      auto *row = new QWidget();
      row->setStyleSheet("background: transparent;");
      auto *hLayout = new QHBoxLayout(row);
      hLayout->setContentsMargins(4, 1, 8, 1);
      hLayout->setSpacing(8);

      auto *cb = new QCheckBox(track.label, row);
      cb->setObjectName("trackCb");
      cb->setChecked(!hadWidgetItems ||
                     checkedTracks.contains(track.streamIndex));
      cb->setStyleSheet("color: #d0dce8;");

      const int initVol = savedVolumes.value(track.streamIndex, 100);
      auto *volSlider = new QSlider(Qt::Horizontal, row);
      volSlider->setObjectName("trackVol");
      volSlider->setRange(0, 200);
      volSlider->setValue(initVol);
      volSlider->setFixedWidth(70);
      volSlider->setToolTip("Track volume");

      auto *volLabel = new QLabel(QString("%1%").arg(initVol), row);
      volLabel->setObjectName("trackVolLabel");
      volLabel->setFixedWidth(36);
      volLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
      volLabel->setStyleSheet("color: #8899aa; font-size: 10px;");

      hLayout->addWidget(cb, 1);
      hLayout->addWidget(volSlider);
      hLayout->addWidget(volLabel);

      connect(cb, &QCheckBox::checkStateChanged, this,
              [this]() { handleAudioSelectionChanged(); });
      connect(volSlider, &QSlider::valueChanged, this, [this, volLabel](int v) {
        volLabel->setText(QString("%1%").arg(v));
        syncPreviewAudioSelection();
      });

      audioTrackList_->setItemWidget(item, row);
    }
    audioTrackList_->setEnabled(true);
    selectAllAudioButton_->setEnabled(true);
    deselectAllAudioButton_->setEnabled(true);
    setupAuxPlayersForSource(path);
    syncPreviewAudioSelection();
  }

  void syncPreviewAudioSelection() {
    if (!player_)
      return;

    const auto selectedStreams = checkedAudioStreamIndices();
    QSet<int> selectedSet(selectedStreams.begin(), selectedStreams.end());

    if (cachedAudioTracks_.isEmpty() || selectedStreams.isEmpty()) {
      if (mainGainSink_)
        mainGainSink_->setMuted(true);
      for (auto *s : auxGainSinks_)
        s->setMuted(true);
      return;
    }

    // Main player always handles audio track index 0 (first in file)
    const int firstStreamIdx = cachedAudioTracks_.first().streamIndex;
    if (mainGainSink_) {
      mainGainSink_->setMuted(!selectedSet.contains(firstStreamIdx));
      mainGainSink_->setVolume(globalVolume_ * getTrackVolumeAtIndex(0));
    }
    if (player_->activeAudioTrack() != 0)
      player_->setActiveAudioTrack(0);

    // Aux players handle tracks 1..N-1. Mute/unmute + per-track volume.
    for (int i = 0; i < auxAudioPlayers_.size(); ++i) {
      const int trackIdx = i + 1;
      if (trackIdx >= cachedAudioTracks_.size())
        break;
      const int streamIdx = cachedAudioTracks_.at(trackIdx).streamIndex;
      auxGainSinks_.at(i)->setMuted(!selectedSet.contains(streamIdx));
      auxGainSinks_.at(i)->setVolume(globalVolume_ *
                                     getTrackVolumeAtIndex(trackIdx));
    }
  }

  void setupAuxPlayersForSource(const QString &sourcePath) {
    destroyAuxAudioPlayers();
    if (cachedAudioTracks_.size() <= 1 || sourcePath.isEmpty())
      return;

    for (int i = 1; i < cachedAudioTracks_.size(); ++i) {
      auto *gainSink = new GainAudioSink(this);
      gainSink->setVolume(mainGainSink_ ? mainGainSink_->volume() : 1.0f);
      gainSink->setMuted(true);

      auto *p = new QMediaPlayer(this);
      gainSink->attachTo(p);
      auto *dummyAudioOutput = new QAudioOutput(p);
      dummyAudioOutput->setVolume(0.0f);
      p->setAudioOutput(dummyAudioOutput);
      p->setSource(QUrl::fromLocalFile(sourcePath));

      const int trackIndex = i;
      connect(p, &QMediaPlayer::mediaStatusChanged, this,
              [p, trackIndex](QMediaPlayer::MediaStatus st) {
                if (st == QMediaPlayer::LoadedMedia ||
                    st == QMediaPlayer::BufferedMedia) {
                  if (p->activeAudioTrack() != trackIndex)
                    p->setActiveAudioTrack(trackIndex);
                }
              });

      auxAudioPlayers_.push_back(p);
      auxGainSinks_.push_back(gainSink);
    }
  }

  void destroyAuxAudioPlayers() {
    for (auto *p : auxAudioPlayers_) {
      p->stop();
      p->deleteLater();
    }
    auxAudioPlayers_.clear();
    for (auto *s : auxGainSinks_)
      s->deleteLater();
    auxGainSinks_.clear();
  }

  void syncAuxPlaybackState() {
    if (!player_)
      return;
    const auto state = player_->playbackState();
    const qint64 pos = player_->position();
    for (auto *p : auxAudioPlayers_) {
      if (state == QMediaPlayer::PlayingState) {
        if (std::llabs(p->position() - pos) > 200)
          p->setPosition(pos);
        if (p->playbackState() != QMediaPlayer::PlayingState)
          p->play();
      } else if (state == QMediaPlayer::PausedState) {
        p->pause();
        p->setPosition(pos);
      } else {
        p->stop();
      }
    }
  }

  void syncAuxPosition(qint64 pos) {
    for (auto *p : auxAudioPlayers_) {
      if (std::llabs(p->position() - pos) > 200)
        p->setPosition(pos);
    }
  }

  void syncAuxVolume(float vol) {
    globalVolume_ = vol;
    syncPreviewAudioSelection();
  }

  void restoreDefaultAudioSelection() { syncPreviewAudioSelection(); }

  void startVirtualPreview(const Request &req) {
    previewModeEnabled_ = true;
    previewStartMs_ = static_cast<qint64>(mmssToSeconds(req.startMmss)) * 1000;
    previewEndMs_ =
        req.endMmss.isEmpty()
            ? static_cast<qint64>(std::llround(req.mediaInfo.duration * 1000.0))
            : static_cast<qint64>(mmssToSeconds(req.endMmss)) * 1000;
    if (player_->source() != QUrl::fromLocalFile(req.path))
      player_->setSource(QUrl::fromLocalFile(req.path));
    syncPreviewAudioSelection();
    setPlayerControlsEnabled(true);
    setProgressVisible(false);
    requestSeek(0);
    refreshPlaybackTimeline();

    if (req.selectedAudioStreamIndices.isEmpty())
      setStatus(
          "Preview mode enabled. Timeline is limited to the selected segment.");
    else
      setStatus("Preview mode enabled.");
    saveUiSettings();
  }

  void rebuildPreviewFromCurrentSettings() {
    if (!previewModeEnabled_)
      return;

    auto [req, err] = buildRequest(false);
    if (!err.isEmpty()) {
      setStatus(err);
      return;
    }

    startVirtualPreview(req);
  }

  void handleAudioSelectionChanged() { syncPreviewAudioSelection(); }

  void updateButtonStates() {
    QString path = videoPathEdit_->text().trimmed();
    QString mode = modeCombo_->currentData().toString();
    bool canRun = !path.isEmpty();
    if (mode == "target_size")
      canRun = canRun && !maxSizeEdit_->text().trimmed().isEmpty();
    if (mode == "preset")
      canRun = canRun && !presetVideoKbpsEdit_->text().trimmed().isEmpty() &&
               !presetAudioKbpsEdit_->text().trimmed().isEmpty();
    runButton_->setEnabled(canRun);
    previewButton_->setEnabled(!path.isEmpty());
    bool hasMedia = !path.isEmpty() && QFileInfo::exists(path);
    markStartButton_->setEnabled(hasMedia);
    markEndButton_->setEnabled(hasMedia);
  }

  void refreshMediaLabel() {
    QString path = videoPathEdit_->text().trimmed();
    if (!path.isEmpty() && QFileInfo::exists(path)) {
      MediaInfo info = probeMediaInfo(path);
      if (info.ok) {
        double mb = QFileInfo(path).size() / (1024.0 * 1024.0);
        infoLabel_->setText(QString("Input: %1x%2  |  %3 min %4 sec  |  %5 MB")
                                .arg(info.width)
                                .arg(info.height)
                                .arg(static_cast<int>(info.duration) / 60)
                                .arg(static_cast<int>(info.duration) % 60)
                                .arg(QString::number(mb, 'f', 1)));
        infoLabel_->setVisible(true);
        return;
      }
    }
    infoLabel_->clear();
    infoLabel_->setVisible(false);
  }

  void updateUiState() {
    updateButtonStates();
    refreshMediaLabel();
    refreshAudioTrackList();
  }

  std::pair<Request, QString> buildRequest(bool requireSize) {
    Request req;
    req.path = videoPathEdit_->text().trimmed();
    if (!QFileInfo::exists(req.path))
      return {req, "File not found"};
    req.compressMode = modeCombo_->currentData().toString();
    if (req.compressMode == "target_size") {
      req.targetMb = parseSizeMb(maxSizeEdit_->text().trimmed());
      if (requireSize && req.targetMb <= 0)
        return {req, "Invalid max size"};
    }
    req.mediaInfo = probeMediaInfo(req.path);
    if (!req.mediaInfo.ok)
      return {req, "Unable to read video information"};
    for (int i = 0; i < audioTrackList_->count(); ++i) {
      auto *item = audioTrackList_->item(i);
      if (!item || !(item->flags() & Qt::ItemIsUserCheckable))
        continue;
      if (item->checkState() == Qt::Checked)
        req.selectedAudioStreamIndices.push_back(
            item->data(Qt::UserRole).toInt());
    }

    QString startRaw = startEdit_->text().trimmed();
    QString endRaw = endEdit_->text().trimmed();
    req.startMmss = startRaw.isEmpty() ? "00:00" : padTime(startRaw);
    if (req.startMmss.isEmpty())
      return {req, "Invalid start time"};
    int startSec = mmssToSeconds(req.startMmss);
    if (startSec >= req.mediaInfo.duration)
      return {req, "Start time exceeds video duration"};

    if (!endRaw.isEmpty()) {
      req.endMmss = padTime(endRaw);
      if (req.endMmss.isEmpty())
        return {req, "Invalid end time"};
      int endSec = mmssToSeconds(req.endMmss);
      req.durationSeconds = endSec - startSec;
      if (req.durationSeconds <= 0)
        return {req, "End time must be greater than start time"};
      req.targetDuration = req.durationSeconds;
    } else
      req.targetDuration =
          qMax(static_cast<int>(req.mediaInfo.duration) - startSec, 1);

    if (req.compressMode == "preset") {
      bool okV = false, okA = false, okW = true;
      req.presetVideoKbps = presetVideoKbpsEdit_->text().trimmed().toInt(&okV);
      req.presetAudioKbps = presetAudioKbpsEdit_->text().trimmed().toInt(&okA);
      QString wr = presetWidthLimitEdit_->text().trimmed();
      req.presetWidthLimit = wr.isEmpty() ? 0 : wr.toInt(&okW);
      if (!okV || !okA || !okW)
        return {req, "Invalid preset values"};
      if (req.presetVideoKbps <= 0 || req.presetAudioKbps <= 0)
        return {req, "Invalid preset bitrates"};
      if (req.presetWidthLimit < 0)
        return {req, "Invalid max width"};
      req.presetFfmpegPreset = presetFfmpegCombo_->currentText();
    }
    req.targetDuration = qMax(req.targetDuration, 1);
    return {req, {}};
  }

  void loadVideoFile(const QString &path) {
    if (previewRefreshTimer_)
      previewRefreshTimer_->stop();
    videoPathEdit_->setText(path);
    settings_.setValue("last_video_dir", QFileInfo(path).absolutePath());
    cleanupPreviewTemp();
    previewStartMs_ = 0;
    previewEndMs_ = -1;
    autoEnteredPreviewMode_ = false;
    player_->setSource(QUrl::fromLocalFile(path));
    player_->setPosition(0);
    pendingSeekPosition_ = 0;
    pendingSeekDirection_ = 0;
    setPlayerControlsEnabled(true);
    setStatus("Video loaded. Source preview is ready. Use playback controls.");
    progressBar_->setValue(0);
    setProgressVisible(false);
    refreshAudioTrackList();
  }

  void selectVideo() {
    if (previewRefreshTimer_)
      previewRefreshTimer_->stop();
    QString path = QFileDialog::getOpenFileName(
        this, "Choose a video",
        settings_.value("last_video_dir", "").toString(),
        "Videos (*.mp4 *.mov *.mkv *.avi *.asf)");
    if (path.isEmpty())
      return;
    loadVideoFile(path);
  }

  void startPreview() {
    QString src = videoPathEdit_->text().trimmed();
    if (!QFileInfo::exists(src)) {
      setStatus("File not found");
      return;
    }

    if (previewModeEnabled_) {
      if (previewRefreshTimer_)
        previewRefreshTimer_->stop();
      previewModeEnabled_ = false;
      previewStartMs_ = 0;
      previewEndMs_ = -1;
      pendingSeekPosition_.reset();
      pendingSeekDirection_ = 0;
      if (player_->source() != QUrl::fromLocalFile(src))
        player_->setSource(QUrl::fromLocalFile(src));
      restoreDefaultAudioSelection();
      refreshPlaybackTimeline();
      setStatus("Preview mode disabled.");
      saveUiSettings();
      return;
    }

    auto [req, err] = buildRequest(false);
    if (!err.isEmpty()) {
      setStatus(err);
      return;
    }
    startVirtualPreview(req);
  }

  void startCutAndCompress() {
    auto [req, err] = buildRequest(true);
    if (!err.isEmpty()) {
      setStatus(err);
      return;
    }
    QString s = QString(req.startMmss).replace(':', '-');
    QString e =
        req.endMmss.isEmpty() ? "end" : QString(req.endMmss).replace(':', '-');
    QString mode = req.compressMode == "target_size" ? "compressed_size"
                   : req.compressMode == "preset"    ? "compressed_preset"
                                                     : "cut_source";
    QString out = buildOutputPath(req.path, s, e + "_" + mode);
    pendingOutputPath_ = out;
    if (openOutputButton_)
      openOutputButton_->setVisible(false);

    progressBar_->setValue(0);
    setProgressVisible(true);
    setStatus("Preparing compression...");
    runButton_->setEnabled(false);
    compressionWorker_ = new CompressionWorker(req, out, this);
    connect(compressionWorker_, &CompressionWorker::progress, progressBar_,
            &QProgressBar::setValue);
    connect(compressionWorker_, &CompressionWorker::status, this,
            [this](const QString &s) { setStatus(s); });
    connect(compressionWorker_, &CompressionWorker::finishedOk, this,
            &MainWindow::onCompressOk);
    connect(compressionWorker_, &CompressionWorker::failed, this,
            &MainWindow::onCompressFailed);
    connect(compressionWorker_, &QThread::finished, this, [this]() {
      runButton_->setEnabled(true);
      compressionWorker_->deleteLater();
      compressionWorker_ = nullptr;
    });
    compressionWorker_->start();
  }

  void onCompressOk(const QString &msg) {
    lastOutputPath_ = pendingOutputPath_;
    pendingOutputPath_.clear();
    setStatus(msg);
    progressBar_->setValue(100);
    setProgressVisible(false);
    if (openOutputButton_ && !lastOutputPath_.isEmpty())
      openOutputButton_->setVisible(true);
  }
  void onCompressFailed(const QString &msg) {
    pendingOutputPath_.clear();
    setStatus(msg);
    if (progressBar_->value() >= 100)
      progressBar_->setValue(0);
    setProgressVisible(false);
    if (openOutputButton_)
      openOutputButton_->setVisible(false);
  }

  void togglePlayPause() {
    if (player_->source().isEmpty())
      return;

    if (player_->playbackState() == QMediaPlayer::PlayingState) {
      player_->pause();
      return;
    }

    const qint64 endPos =
        previewModeEnabled_ ? effectivePreviewEnd() : player_->duration();
    const qint64 restartPos = previewModeEnabled_ ? effectivePreviewStart() : 0;
    if (player_->position() >= qMax<qint64>(0, endPos - 80))
      player_->setPosition(restartPos);
    player_->play();
  }
  void stopPlayback() {
    if (player_->source().isEmpty())
      return;
    player_->stop();
    pendingSeekPosition_ = previewModeEnabled_ ? effectivePreviewStart() : 0;
    pendingSeekDirection_ = 0;
    positionSlider_->setValue(0);
    const qint64 duration =
        previewModeEnabled_ ? effectiveTimelineDuration() : player_->duration();
    timeLabel_->setText("00:00 / " + formatMs(duration));
    if (previewModeEnabled_)
      player_->setPosition(effectivePreviewStart());
  }
  void onPlaybackStateChanged(QMediaPlayer::PlaybackState st) {
    bool playing = st == QMediaPlayer::PlayingState;
    playPauseButton_->setText(playing ? "Pause" : "Play");
    playPauseButton_->setIcon(playing ? pauseIcon_ : playIcon_);
  }
  void onDurationChanged(qint64 d) {
    Q_UNUSED(d);
    refreshPlaybackTimeline();
  }

  void onPositionChanged(qint64 p) {
    if (previewModeEnabled_) {
      const qint64 start = effectivePreviewStart();
      const qint64 end = effectivePreviewEnd();
      if (p < start) {
        player_->setPosition(start);
        return;
      }
      if (p > end) {
        player_->pause();
        player_->setPosition(end);
        p = end;
      }
    }
    if (pendingSeekPosition_.has_value()) {
      qint64 target = *pendingSeekPosition_;
      if (std::llabs(p - target) <= 80) {
        pendingSeekPosition_.reset();
        pendingSeekDirection_ = 0;
        lastSeekRetryMs_ = 0;
      } else {
        p = target;
        qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (!sliderDragging_ && (now - lastSeekRetryMs_) >= 120) {
          player_->setPosition(target);
          lastSeekRetryMs_ = now;
        }
      }
    }
    const qint64 displayPos = displayPositionFromAbsolute(p);
    if (!sliderDragging_)
      positionSlider_->setValue(static_cast<int>(displayPos));
    updateTimeLabel(displayPos, previewModeEnabled_
                                    ? effectiveTimelineDuration()
                                    : player_->duration());
  }

  void onSliderPressed() { sliderDragging_ = true; }
  void onSliderReleased() {
    sliderDragging_ = false;
    requestSeek(positionSlider_->value());
  }
  void onSliderMoved(int p) {
    pendingSeekPosition_ = absolutePositionFromDisplay(p);
    updateTimeLabel(p, previewModeEnabled_ ? effectiveTimelineDuration()
                                           : player_->duration());
  }

  void seekRelative(qint64 off) {
    if (player_->source().isEmpty())
      return;
    const qint64 currentPos = displayPositionFromAbsolute(player_->position());
    const qint64 maxPos =
        previewModeEnabled_ ? effectiveTimelineDuration() : player_->duration();
    requestSeek(qBound<qint64>(0, currentPos + off, maxPos));
  }

  void markStartFromPlayer() {
    if (player_->source().isEmpty()) {
      setStatus("Load a video first.");
      return;
    }
    QString m = formatMs(player_->position());
    startEdit_->setText(
        QString("%1:%2").arg(m.left(2).toInt()).arg(m.mid(3, 2)));
    setStatus("Start marked at " + m + ".");
  }
  void markEndFromPlayer() {
    if (player_->source().isEmpty()) {
      setStatus("Load a video first.");
      return;
    }
    QString m = formatMs(player_->position());
    endEdit_->setText(QString("%1:%2").arg(m.left(2).toInt()).arg(m.mid(3, 2)));
    setStatus("End marked at " + m + ".");
  }

  void requestSeek(qint64 target) {
    const qint64 maxPos =
        previewModeEnabled_ ? effectiveTimelineDuration() : player_->duration();
    qint64 t = qBound<qint64>(0, target, maxPos);
    const qint64 absoluteTarget = absolutePositionFromDisplay(t);
    qint64 cur = player_->position();
    pendingSeekDirection_ =
        absoluteTarget > cur ? 1 : (absoluteTarget < cur ? -1 : 0);
    pendingSeekPosition_ = absoluteTarget;
    lastSeekRetryMs_ = 0;
    player_->setPosition(absoluteTarget);
  }

  void updateTimeLabel(qint64 p, qint64 d) {
    timeLabel_->setText(QString("%1 / %2").arg(formatMs(p), formatMs(d)));
  }

  void onTrimInputsChanged() {
    if (previewModeEnabled_) {
      auto [req, err] = buildRequest(false);
      if (err.isEmpty()) {
        previewStartMs_ =
            static_cast<qint64>(mmssToSeconds(req.startMmss)) * 1000;
        previewEndMs_ =
            req.endMmss.isEmpty()
                ? static_cast<qint64>(
                      std::llround(req.mediaInfo.duration * 1000.0))
                : static_cast<qint64>(mmssToSeconds(req.endMmss)) * 1000;
        const qint64 absPos = player_ ? player_->position() : 0;
        if (absPos < effectivePreviewStart() || absPos > effectivePreviewEnd())
          requestSeek(0);
        else
          refreshPlaybackTimeline();
      }
    } else {
      updateTrimMarkers();
    }
  }

  void updateTrimMarkers() {
    if (!positionSlider_)
      return;

    if (previewModeEnabled_) {
      positionSlider_->setTrimMarkers(-1, -1);
      return;
    }

    int startMs = -1;
    int endMs = -1;

    const QString rawStartValue =
        startEdit_ ? startEdit_->text().trimmed() : QString();
    const QString startValue = padTime(rawStartValue);
    if (!startValue.isEmpty())
      startMs = mmssToSeconds(startValue) * 1000;

    const QString rawEndValue =
        endEdit_ ? endEdit_->text().trimmed() : QString();
    const QString endValue = padTime(rawEndValue);
    if (!endValue.isEmpty())
      endMs = mmssToSeconds(endValue) * 1000;

    positionSlider_->setTrimMarkers(startMs, endMs);
  }

  void setStatus(const QString &msg) {
    statusLabel_->setText("Status: " + msg);
  }
  void setProgressVisible(bool visible) {
    if (progressBar_)
      progressBar_->setVisible(visible);
  }

  void openLastOutputFolder() {
    if (lastOutputPath_.isEmpty())
      return;
#ifdef Q_OS_WIN
    if (QFileInfo::exists(lastOutputPath_)) {
      QProcess::startDetached(
          "explorer.exe",
          {"/select,", QDir::toNativeSeparators(lastOutputPath_)});
      return;
    }
#endif
    const QString folder = QFileInfo(lastOutputPath_).absolutePath();
    if (!folder.isEmpty())
      QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
  }

  void cleanupPreviewTemp() {
    if (previewRefreshTimer_)
      previewRefreshTimer_->stop();
    if (player_)
      player_->stop();
    destroyAuxAudioPlayers();
    const bool hadPreviewMode = previewModeEnabled_;
    previewModeEnabled_ = false;
    autoEnteredPreviewMode_ = false;
    pendingPreviewAutoPlay_ = false;
    pendingPreviewDisplayPos_ = 0;
    previewStartMs_ = 0;
    previewEndMs_ = -1;
    pendingSeekPosition_.reset();
    pendingSeekDirection_ = 0;
    lastSeekRetryMs_ = 0;
    if (positionSlider_) {
      positionSlider_->setEnabled(false);
      positionSlider_->setRange(0, 0);
    }
    if (timeLabel_)
      timeLabel_->setText("00:00 / 00:00");
    if (playPauseButton_)
      setPlayerControlsEnabled(false);
    updatePreviewButtonText();
    updateTrimMarkers();
    if (hadPreviewMode)
      saveUiSettings();
  }

  QSettings settings_;
  QString cachedMediaPath_;
  QString cachedAudioPath_;
  QString pendingOutputPath_;
  QString lastOutputPath_;
  MediaInfo cachedMediaInfo_;
  QVector<AudioTrackInfo> cachedAudioTracks_;
  QTimer *mediaInfoTimer_ = nullptr;
  QTimer *previewRefreshTimer_ = nullptr;
  CompressionWorker *compressionWorker_ = nullptr;
  GainAudioSink *mainGainSink_ = nullptr;
  QVector<QMediaPlayer *> auxAudioPlayers_;
  QVector<GainAudioSink *> auxGainSinks_;
  bool sliderDragging_ = false;
  float globalVolume_ = 1.0f;
  bool previewModeEnabled_ = false;
  bool autoEnteredPreviewMode_ = false;
  bool pendingPreviewAutoPlay_ = false;
  std::optional<qint64> pendingSeekPosition_;
  int pendingSeekDirection_ = 0;
  qint64 lastSeekRetryMs_ = 0;
  qint64 previewStartMs_ = 0;
  qint64 previewEndMs_ = -1;
  qint64 pendingPreviewDisplayPos_ = 0;

  QLineEdit *videoPathEdit_, *startEdit_, *endEdit_, *maxSizeEdit_,
      *presetVideoKbpsEdit_, *presetAudioKbpsEdit_, *presetWidthLimitEdit_;
  QComboBox *modeCombo_, *presetCombo_, *presetFfmpegCombo_;
  QListWidget *audioTrackList_ = nullptr;
  QLabel *maxSizeLabel_, *infoLabel_, *timeLabel_, *statusLabel_;
  QLabel *previewIndicatorLabel_ = nullptr;
  QFrame *previewBadgeWrap_ = nullptr;
  QPushButton *markStartButton_, *markEndButton_, *runButton_, *previewButton_,
      *backButton_, *playPauseButton_, *stopButton_, *forwardButton_,
      *openOutputButton_ = nullptr, *previewBadgeCloseButton_ = nullptr,
      *selectAllAudioButton_ = nullptr, *deselectAllAudioButton_ = nullptr;
  QWidget *modeSectionWrap_ = nullptr, *sizeRowWrap_ = nullptr,
          *presetWrap_ = nullptr;
  QVideoWidget *videoWidget_ = nullptr;
  SeekSlider *positionSlider_;
  QPushButton *volumeButton_ = nullptr;
  QWidget *volumePopup_ = nullptr;
  QSlider *volumeSliderPopup_ = nullptr;
  QLabel *volumeLabelPopup_ = nullptr;
  QProgressBar *progressBar_;
  QIcon playIcon_, pauseIcon_, volumeIcon_, mutedIcon_;

  QMediaPlayer *player_ = nullptr;
};

int main(int argc, char *argv[]) {
#ifdef Q_OS_WIN
  constexpr int kWinIconResourceId = 101;
  using SetAppIdFn = HRESULT(WINAPI *)(PCWSTR);
  HMODULE shell32 = LoadLibraryW(L"shell32.dll");
  if (shell32) {
    auto setAppId = reinterpret_cast<SetAppIdFn>(
        GetProcAddress(shell32, "SetCurrentProcessExplicitAppUserModelID"));
    if (setAppId) {
      setAppId(L"videocutandcompress.app");
    }
    FreeLibrary(shell32);
  }
#endif
  QApplication app(argc, argv);
  app.setStyle("Fusion");
  app.setStyleSheet(APP_QSS);
  app.setWindowIcon(QIcon(":/icons/app.ico"));
  MainWindow w;
  w.setWindowIcon(QIcon(":/icons/app.ico"));
  w.show();
#ifdef Q_OS_WIN
  HWND hwnd = reinterpret_cast<HWND>(w.winId());
  HINSTANCE hinst = GetModuleHandleW(nullptr);
  HICON iconBig =
      static_cast<HICON>(LoadImageW(hinst, MAKEINTRESOURCEW(kWinIconResourceId),
                                    IMAGE_ICON, 256, 256, LR_DEFAULTCOLOR));
  HICON iconSmall =
      static_cast<HICON>(LoadImageW(hinst, MAKEINTRESOURCEW(kWinIconResourceId),
                                    IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR));
  if (iconBig)
    SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(iconBig));
  if (iconSmall)
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL,
                 reinterpret_cast<LPARAM>(iconSmall));
#endif
  return app.exec();
}

#if !defined(__INTELLISENSE__) && !defined(__clang__)
#include "main.moc"
#endif
