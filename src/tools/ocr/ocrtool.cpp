// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 FlameshotAI Contributors

#include "ocrtool.h"
#include "core/flameshotdaemon.h"

#include <QGuiApplication>
#include <QMessageBox>
#include <QProcess>
#include <QRegularExpression>
#include <QTemporaryDir>

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

QString runEngine(const QString& program,
                  const QStringList& args,
                  bool parseRepr)
{
    QProcess process;
    process.start(program, args);
    if (!process.waitForStarted(kStartTimeoutMs)) {
        return {};
    }
    if (!process.waitForFinished(kFinishTimeoutMs)) {
        process.kill();
        process.waitForFinished();
        return {};
    }
    if (process.exitStatus() != QProcess::NormalExit ||
        process.exitCode() != 0) {
        return {};
    }
    const QString output =
      QString::fromUtf8(process.readAllStandardOutput()).trimmed();
    return parseRepr ? textsFromRepr(output) : output;
}

// shortcut: engine command lines are hardcoded, revisit when a CLI changes
QString runOcr(const QString& imagePath)
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
        { "paddleocr", { "--image_dir", imagePath }, true },
        // tesseract prints plain text and ships with Debian
        { "tesseract", { imagePath, "stdout", "-l", "chi_sim+eng" }, false },
        { "tesseract", { imagePath, "stdout" }, false },
    };

    for (const auto& engine : engines) {
        const QString text =
          runEngine(QString::fromLatin1(engine.program), engine.args,
                    engine.parseRepr);
        if (!text.isEmpty()) {
            return text;
        }
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
    return tr("Extract text and copy it to clipboard");
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
    const QString text = runOcr(path);
    QGuiApplication::restoreOverrideCursor();

    if (text.isEmpty()) {
        QMessageBox::warning(
          nullptr,
          tr("OCR"),
          tr("No text found. Install rapidocr, paddleocr or tesseract."));
        return;
    }

    // Go through the daemon so the clipboard survives this process exiting.
    FlameshotDaemon::copyToClipboard(text, tr("Text copied to clipboard"));

    emit requestAction(REQ_CLEAR_SELECTION);
    emit requestAction(REQ_CAPTURE_DONE_OK);
    emit requestAction(REQ_CLOSE_GUI);
}
