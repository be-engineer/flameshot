// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 FlameshotOCR Contributors

#include "ocrtool.h"
#include "utils/confighandler.h"
#include "utils/filenamehandler.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMessageBox>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextStream>

namespace {

constexpr int kStartTimeoutMs = 2000;
constexpr int kFinishTimeoutMs = 30000;

// The Python based engines print their result as a Python repr, e.g.
// [[[[0, 0], ...], 'some text', 0.98], ...]. Pull out the quoted fields,
// which are the recognised lines.
// shortcut: text containing both quote kinds gets split, fix if it shows up
QString textsFromRepr(const QString& output)
{
    static const QRegularExpression quoted(
      QStringLiteral("\"([^\"]*)\"|'([^']*)'"));
    QStringList texts;
    auto it = quoted.globalMatch(output);
    while (it.hasNext()) {
        const auto match = it.next();
        const QString text = match.captured(1) + match.captured(2);
        if (!text.trimmed().isEmpty()) {
            texts << text;
        }
    }
    return texts.join(QLatin1Char('\n'));
}

// Desktop launched apps often miss the shell PATH, so pip/pipx installs in
// ~/.local/bin are invisible to QProcess. Look there explicitly.
QString resolveEngine(const QString& program)
{
    QString found = QStandardPaths::findExecutable(program);
    if (!found.isEmpty()) {
        return found;
    }
    const QStringList extraDirs = {
        QDir::homePath() + QStringLiteral("/.local/bin"),
        QStringLiteral("/usr/local/bin"),
    };
    for (const QString& dir : extraDirs) {
        const QString path = dir + QLatin1Char('/') + program;
        if (QFileInfo(path).isExecutable()) {
            return path;
        }
    }
    return {};
}

// Returns the recognised text, or an empty string on failure. `engine` is set
// to the name of the engine that produced the text, `error` to why it failed.
QString runEngine(const QString& program,
                  const QStringList& args,
                  bool parseRepr,
                  QString* engine,
                  QString* error)
{
    QProcess process;
    process.start(program, args);
    if (!process.waitForStarted(kStartTimeoutMs)) {
        *error = QStringLiteral("not found or not startable");
        return {};
    }
    if (!process.waitForFinished(kFinishTimeoutMs)) {
        process.kill();
        process.waitForFinished();
        *error = QStringLiteral("timed out");
        return {};
    }
    const QString output =
      QString::fromUtf8(process.readAllStandardOutput()).trimmed();
    const QString errOutput =
      QString::fromUtf8(process.readAllStandardError()).trimmed();

    if (process.exitStatus() != QProcess::NormalExit ||
        process.exitCode() != 0) {
        // Engines report the real problem on stderr (python traceback etc).
        QString errLine = errOutput.split(QLatin1Char('\n')).last().trimmed();
        if (errLine.size() > 160) {
            errLine = errLine.left(160) + QStringLiteral("...");
        }
        *error = QStringLiteral("exit code %1%2")
                   .arg(process.exitCode())
                   .arg(errLine.isEmpty()
                          ? QString()
                          : QStringLiteral(": ") + errLine);
        if (errLine.contains(QStringLiteral("ImportError")) ||
            errLine.contains(QStringLiteral("ModuleNotFoundError"))) {
            *error += QStringLiteral(
              "\nFix: pipx inject rapidocr onnxruntime "
              "(or pip install --user rapidocr onnxruntime)");
        }
        return {};
    }

    QString text = parseRepr ? textsFromRepr(output) : output;
    if (text.isEmpty() && parseRepr && !errOutput.isEmpty()) {
        // Some builds print the result on stderr.
        text = textsFromRepr(errOutput);
    }
    if (text.isEmpty()) {
        // Show what the engine actually printed so failures are diagnosable.
        QString snippet = (output.isEmpty() ? errOutput : output).simplified();
        if (snippet.size() > 160) {
            snippet = snippet.left(160) + QStringLiteral("...");
        }
        *error = snippet.isEmpty()
                   ? QStringLiteral("no output")
                   : QStringLiteral("no text recognised: %1").arg(snippet);
        return {};
    }
    if (engine) {
        *engine = QFileInfo(program).fileName();
    }
    return text;
}

// shortcut: engine command lines are hardcoded, revisit when a CLI changes
QString runOcr(const QString& imagePath, QString* engine, QString* errors)
{
    struct Engine
    {
        const char* program;
        QStringList args;
        bool parseRepr;
    };
    const QList<Engine> engines = {
        { "rapidocr_onnxruntime", { "-img", imagePath }, true },
        { "rapidocr", { "-img", imagePath }, true },
        // paddleocr 3.x takes a subcommand; 2.x only knows --image_dir
        { "paddleocr", { "ocr", "-i", imagePath }, true },
        { "paddleocr", { "--image_dir", imagePath }, true },
        // tesseract prints plain text and ships with Debian
        { "tesseract", { imagePath, "stdout", "-l", "chi_sim+eng" }, false },
        { "tesseract", { imagePath, "stdout" }, false },
    };

    for (const auto& engineDef : engines) {
        const QString program =
          resolveEngine(QString::fromLatin1(engineDef.program));
        if (program.isEmpty()) {
            *errors += QStringLiteral("%1: not installed; ")
                         .arg(engineDef.program);
            continue;
        }
        QString error;
        const QString text = runEngine(program, engineDef.args,
                                       engineDef.parseRepr, engine, &error);
        if (!text.isEmpty()) {
            return text;
        }
        *errors +=
          QStringLiteral("%1: %2; ").arg(engineDef.program, error);
    }
    return {};
}

} // namespace

OcrTool::OcrTool(QObject* parent)
  : AbstractActionTool(parent)
{}

bool OcrTool::closeOnButtonPressed() const
{
    return true;
}

QIcon OcrTool::icon(const QColor& background, bool inEditor) const
{
    Q_UNUSED(inEditor)
    return QIcon(iconPath(background) + "ocr.svg");
}

QString OcrTool::name() const
{
    return tr("OCR");
}

QString OcrTool::description() const
{
    return tr("Extract text and save it to a file");
}

CaptureTool::Type OcrTool::type() const
{
    return CaptureTool::TYPE_OCR;
}

CaptureTool* OcrTool::copy(QObject* parent)
{
    return new OcrTool(parent);
}

void OcrTool::pressed(CaptureContext& context)
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("ocr.png"));
    if (!dir.isValid() || !context.selectedScreenshotArea().save(path, "PNG")) {
        QMessageBox::warning(nullptr, tr("OCR"), tr("Could not save capture."));
        return;
    }

    QGuiApplication::setOverrideCursor(Qt::WaitCursor);
    QElapsedTimer timer;
    timer.start();
    QString engine;
    QString errors;
    const QString text = runOcr(path, &engine, &errors);
    QGuiApplication::restoreOverrideCursor();

    if (text.isEmpty()) {
        QMessageBox::warning(
          nullptr,
          tr("OCR"),
          tr("No text found. Install rapidocr, paddleocr or tesseract.") +
            QStringLiteral("\n\n") +
            errors.trimmed());
        return;
    }

    // Save beside the screenshots instead of using the clipboard, which is
    // lost when the hosting daemon exits.
    FileNameHandler names;
    const QString outPath = names.properScreenshotPath(
      ConfigHandler().savePath(), QStringLiteral("txt"));
    QFile out(outPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(
          nullptr, tr("OCR"), tr("Could not write %1").arg(outPath));
        return;
    }
    QTextStream stream(&out);
    stream << text;
    out.close();

    QMessageBox::information(nullptr,
                             tr("OCR"),
                             tr("Text saved to %1\n(%2, %3s)")
                               .arg(outPath, engine)
                               .arg(timer.elapsed() / 1000.0, 0, 'f', 1));

    emit requestAction(REQ_CLEAR_SELECTION);
    emit requestAction(REQ_CAPTURE_DONE_OK);
    emit requestAction(REQ_CLOSE_GUI);
}
