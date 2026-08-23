#ifndef PLAYLISTMODEL_H
#define PLAYLISTMODEL_H

#include <QAbstractListModel>
#include <QVector>
#include <QString>

// One visible playlist entry. Both population paths (tree-based
// updateUIFromCurrentNode and filesystem-based navigateToFolderWithoutHistory)
// build a vector of these and hand it to the model in one shot.
struct PlaylistRow {
    QString name;   // display text, including any emoji prefix (🎵 / 📁 / ..)
    QString path;   // exposed via PathRole (was QListWidgetItem UserRole)
    int     type;   // exposed via TypeRole (was QListWidgetItem UserRole+1)

    PlaylistRow() : type(0) {}
    PlaylistRow(const QString &n, const QString &p, int t)
        : name(n), path(p), type(t) {}
};

// Flat list model backing the playlist QListView. Only visible delegates are
// rendered by the view, so this scales to 100k+ rows without per-item widgets.
class PlaylistModel : public QAbstractListModel
{
    Q_OBJECT
public:
    enum Roles {
        PathRole = Qt::UserRole,       // matches the old Qt::UserRole usage
        TypeRole = Qt::UserRole + 1    // matches the old Qt::UserRole+1 usage
    };

    explicit PlaylistModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;

    // Bulk replace (model reset). Used by both population paths.
    void setRows(QVector<PlaylistRow> rows);
    void clearRows();
    void appendRow(const PlaylistRow &row);

    // Change one row's display text in place. setRows() is the only other way
    // to alter what is shown, and it resets the model - which throws away the
    // selection and the scroll position and, through
    // updateUIFromCurrentNode(), walks the whole playlist tree. That is far too
    // much for a folder scan's progress counter ticking ten times a second, so
    // this exists to repaint exactly one row. Returns false if the row is gone.
    bool updateRow(int row, const QString &name);

    const QVector<PlaylistRow> &rows() const { return m_rows; }

private:
    QVector<PlaylistRow> m_rows;
};

#endif // PLAYLISTMODEL_H
