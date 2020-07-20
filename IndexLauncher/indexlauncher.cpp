#include "indexlauncher.h"

#include <atomic>
#include <chrono>
#include <vector>

#include <QtCore/QAbstractTableModel>
#include <QtCore/QBitArray>
#include <QtCore/QCoreApplication>
#include <QtCore/QDebug>
#include <QtCore/QFile>
#include <QtCore/QRegularExpression>
#include <QtCore/QVector>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtGui/QScreen>
#include <QtGui/QKeyEvent>
#include <QtGui/QWindow>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QVBoxLayout>

#include "threadpool.h"

using namespace std::literals::chrono_literals;

static const QString kStyleSheets = QStringLiteral("border: none;");
static constexpr int kMinWidth = 500;
static constexpr double kOpacity = 0.8;

static constexpr auto kFrameInterval = 10ms;

inline bool IsValidFile(const QString& file) {
  return (file.length() > 1) && (file.at(0) == QLatin1Char('Q')) &&
         (file.at(1).isUpper());
}

QString FilePath(QString file) {
  if (file.endsWith(QStringLiteral(".md"))) {
    file = file.left(file.length() - 3);
  }
  QChar ch = IsValidFile(file) ? file.at(1) : file.at(0);
  return QStringLiteral("../../%1/%2/%2.md").arg(ch.toUpper()).arg(file);
}

class Model : public QAbstractListModel {
  friend class IndexLauncher;

 public:
  explicit Model(QObject* parent = nullptr);

  void SelectFile(const QString& file = {});
  void Select(const QModelIndex& idx);

  int rowCount(const QModelIndex& parent) const override;
  QVariant data(const QModelIndex& index, int role) const override;

 private:
  QString file_;

  // <File, <Title, (Text, Link)>>
  QMap<QString, QMultiMap<QString, QPair<QString, QString>>> titles_;
  QMultiMap<QString, QPair<QString, QString>>* currentData_ = nullptr;
  QModelIndex currentIndex_;
};

Model::Model(QObject* parent) : QAbstractListModel(parent) {}

void Model::SelectFile(const QString& file) {
  if (file != file_) {
    beginResetModel();
    file_ = file;
    currentData_ = file_.isEmpty() ? nullptr : &titles_[file_];
    endResetModel();
  }
}

void Model::Select(const QModelIndex& idx) {
  auto prev = index(currentIndex_.row(), currentIndex_.column());
  currentIndex_ = index(idx.row(), idx.column());
  emit dataChanged(prev, prev);
  emit dataChanged(currentIndex_, currentIndex_);

  auto list = qobject_cast<QListView*>(parent());
  auto filter = qobject_cast<QSortFilterProxyModel*>(list->model());
  list->scrollTo(filter->mapFromSource(currentIndex_));
}

int Model::rowCount(const QModelIndex&) const {
  return file_.isEmpty() ? titles_.count() : titles_.value(file_).count();
}

QVariant Model::data(const QModelIndex& index, int role) const {
  switch (role) {
    case Qt::DisplayRole:
      if (currentData_) {
        auto it = currentData_->cbegin() + index.row();
        return QStringLiteral("%1\n%2").arg(it.key(), it.value().first);
      } else {
        return (titles_.cbegin() + index.row()).key();
      }

    case Qt::BackgroundRole:
      if (index.row() == currentIndex_.row()) {
        return QColor(QStringLiteral("royalblue"));
      }
      break;

    default:
      break;
  }
  return {};
}

IndexLauncher::IndexLauncher(QWidget* parent, Qt::WindowFlags flags)
    : QFrame(parent, flags),
      input_(new QLineEdit(this)),
      list_(new QListView(this)),
      model_(new Model(list_)),
      filter_(new QSortFilterProxyModel(list_)) {
  auto layout = new QVBoxLayout(this);

  input_->setStyleSheet(kStyleSheets);
  input_->installEventFilter(this);
  layout->addWidget(input_);

  auto line = new QFrame(this);
  line->setFrameShape(QFrame::HLine);
  line->setFrameShadow(QFrame::Sunken);
  layout->addWidget(line);

  filter_->setSourceModel(model_);
  filter_->setFilterCaseSensitivity(Qt::CaseInsensitive);
  list_->setModel(filter_);
  list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  list_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  list_->setStyleSheet(kStyleSheets);
  list_->setSelectionMode(QAbstractItemView::NoSelection);
  layout->addWidget(list_);

  connect(input_, &QLineEdit::textChanged, input_, [this](const QString& text) {
    filter_->setFilterFixedString(text);
    model_->Select(filter_->mapToSource(filter_->index(0, 0)));
  });
}

size_t IndexLauncher::IndexFiles(const QFileInfoList& files) {
  model_->beginResetModel();
  model_->titles_.clear();

  std::vector<std::future<void>> futures;
  std::atomic<size_t> count = ATOMIC_VAR_INIT(0);

  QProgressBar progress;
  progress.setTextVisible(false);
  progress.setMaximum(files.count() - 1);
  progress.setWindowFlags(Qt::Tool | Qt::FramelessWindowHint |
                          Qt::WindowStaysOnTopHint);
  progress.show();

  std::function<void(const QFileInfo&,
                     QMultiMap<QString, QPair<QString, QString>>*)>
      IndexFile;
  IndexFile = [&count](const QFileInfo& fileInfo,
                       QMultiMap<QString, QPair<QString, QString>>* titleMap) {
    QFile file{fileInfo.absoluteFilePath()};
    if (!file.open(QFile::ReadOnly | QFile::Text)) {
      return;
    }

    while (!file.atEnd()) {
      QString line = file.readLine();
      if (!line.startsWith(QLatin1Char('#'))) {
        continue;
      }

      // Trim
      while (line.startsWith(QLatin1Char('#'))) {
        line.remove(0, 1);
      }
      line = line.trimmed();
      // QString raw = line;

      // Remove syntax and strings * = " \ ' /
      // [\*\=\"\\\'\/]
      static const auto kSyntaxRegexp = [] {
        auto regexp =
            QRegularExpression{QStringLiteral("[\\*\\=\\\"\\\\\\'\\/]")};
        regexp.optimize();
        return regexp;
      }();
      line.replace(kSyntaxRegexp, QString{});

      // Replace markdown link [xxx](yyy) to xxx
      // \[(\S+)\]\(\S+\)
      static const auto kLinkRegexp = [] {
        auto regexp =
            QRegularExpression{QStringLiteral("\\[(\\S+)\\]\\(\\S+\\)")};
        regexp.optimize();
        return regexp;
      }();
      line.replace(kLinkRegexp, QStringLiteral("\\1"));
      QString unref = line;

      // Find function name
      //[\w_]+::([\w_]+)\(
      static const auto kMethodRegexp = [] {
        auto regexp =
            QRegularExpression{QStringLiteral("[\\w_]+::([\\w_]+)\\(")};
        regexp.optimize();
        return regexp;
      }();
      QStringList titles;
      QRegularExpressionMatch match = kMethodRegexp.match(line);
      if (match.hasMatch()) {
        titles << match.captured(1);
      }

      // No function name, find other symbols
      // [\w_]+::([\w_]+)
      static const auto kMemberRegexp = [] {
        QRegularExpression regexp{QStringLiteral("[\\w_]+::([\\w_]+)")};
        regexp.optimize();
        return regexp;
      }();
      if (titles.isEmpty()) {
        QRegularExpressionMatchIterator it = kMemberRegexp.globalMatch(line);
        while (it.hasNext()) {
          titles << it.next().captured(1);
        };
      }

      // Generate html link _ , . : ( ) [ ] { }
      // [\_\,\.\:\(\)\[\]\{\}]
      static const auto kPunctuationRegexp = [] {
        QRegularExpression regexp{
            QStringLiteral("[\\_\\,\\.\\:\\(\\)\\[\\]\\{\\}]")};
        regexp.optimize();
        return regexp;
      }();
      line.replace(kPunctuationRegexp, QString{});
      line.replace(QLatin1Char('\t'), QLatin1Char('-'));
      if (titles.isEmpty()) {
        titles = line.split(QLatin1Char(' '), QString::SkipEmptyParts);
      }
      line.replace(QLatin1Char(' '), QLatin1Char('-'));
      line = line.toLower();

      // qDebug().noquote() << "File: " << fileInfo.completeBaseName()
      //                    << "\n Raw:\t" << raw
      //                    << "\n Unref:\t" << unref
      //                    << "\n Titles:\t" << titles
      //                    << "\n link:\t" << line;

      for (auto&& title : titles) {
        titleMap->insert(title, {unref, line});
      }
      count += titles.count();
    }
  };

  // Add tasks to thread pool
  QVector<QMultiMap<QString, QPair<QString, QString>>> titles;
  titles.resize(files.count());
  futures.reserve(files.count());
  auto now = std::chrono::high_resolution_clock::now();
  for (int i = 0; i < files.count(); ++i) {
    futures.emplace_back(
        ThreadPool::default_pool().AddTask(IndexFile, files.at(i), &titles[i]));
    if ((std::chrono::high_resolution_clock::now() - now) > kFrameInterval) {
      QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
      now = std::chrono::high_resolution_clock::now();
    }
  }

  for (int i = 0; i < files.count(); ++i) {
    // Wait future
    std::future_status status;
    do {
      QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
      status = futures[i].wait_for(kFrameInterval);
    } while (status != std::future_status::ready);
    futures[i].get();

    // Add result to model
    model_->titles_[files.at(i).completeBaseName()] = titles.at(i);

    // Update progress
    progress.setValue(progress.value() + 1);
  }

  model_->endResetModel();

  return count.load();
}

void IndexLauncher::Trigger() {
  if (isVisible()) {
    if (model_->file_.isEmpty()) {
      QString filePath = model_->currentIndex_.data().toString();
      QGuiApplication::clipboard()->setText(
          FilePath(filePath.isEmpty() ? input_->text() : filePath));
    }
    hide();
    return;
  }

  model_->SelectFile();
  QString clipBoardText = QGuiApplication::clipboard()->text();
  if (IsValidFile(clipBoardText)) {
    input_->setText(clipBoardText);
  } else {
    input_->clear();
  }
  input_->setPlaceholderText(tr("Please enter file name or class name"));
  emit input_->textChanged({});

  show();
  QWidget::raise();
  input_->setFocus();

  QWindow* window = windowHandle();
  if (window) {
    QScreen* screen = window->screen();
    QRect sGeom = screen->geometry();
    setFixedWidth(std::max(kMinWidth, sGeom.width() / 2));
    QRect geom = geometry();
    geom.moveCenter(sGeom.center());
    setGeometry(geom);
    window->setOpacity(kOpacity);
  }
}

bool IndexLauncher::eventFilter(QObject* object, QEvent* event) {
  static constexpr int kPageIndexes = 5;

  if (event->type() == QEvent::KeyRelease) {
    auto e = dynamic_cast<QKeyEvent*>(event);
    switch (e->key()) {
      case Qt::Key_Escape:
        hide();
        break;

      case Qt::Key_Up: {
        int row = filter_->mapFromSource(model_->currentIndex_).row() - 1;
        row = (row < 0) ? (filter_->rowCount() - 1) : row;
        model_->Select(filter_->mapToSource(filter_->index(row, 0)));
      } break;

      case Qt::Key_Down: {
        int row = filter_->mapFromSource(model_->currentIndex_).row() + 1;
        row %= filter_->rowCount();
        model_->Select(filter_->mapToSource(filter_->index(row, 0)));
      } break;

      case Qt::Key_PageUp: {
        int row =
            filter_->mapFromSource(model_->currentIndex_).row() - kPageIndexes;
        row = (row < 0) ? 0 : row;
        model_->Select(filter_->mapToSource(filter_->index(row, 0)));
      } break;

      case Qt::Key_PageDown: {
        int row =
            filter_->mapFromSource(model_->currentIndex_).row() + kPageIndexes;
        row = (row >= filter_->rowCount()) ? (filter_->rowCount() - 1) : row;
        model_->Select(filter_->mapToSource(filter_->index(row, 0)));
      } break;

      case Qt::Key_Return:
      case Qt::Key_Enter: {
        if (model_->file_.isEmpty()) {
          QString file = model_->currentIndex_.data().toString();
          if (e->modifiers() & Qt::ControlModifier) {
            QGuiApplication::clipboard()->setText(
                FilePath(file.isEmpty() ? input_->text() : file));
            hide();
          } else {
            model_->SelectFile(file);
            input_->setPlaceholderText(tr("Please enter title"));
            input_->clear();
          }
        } else {
          if (e->modifiers() & Qt::ControlModifier) {
            QString title =
                (model_->currentData_->cbegin() + model_->currentIndex_.row())
                    .value()
                    .second;
            QGuiApplication::clipboard()->setText(QStringLiteral("#") + title);
            hide();
          } else {
            QString filePath = FilePath(model_->file_);
            QString title =
                (model_->currentData_->cbegin() + model_->currentIndex_.row())
                    .value()
                    .second;
            QGuiApplication::clipboard()->setText(
                QStringLiteral("%1#%2").arg(filePath, title));
            hide();
          }
        }
      } break;

      default:
        break;
    }
  }
  return QFrame::eventFilter(object, event);
}
