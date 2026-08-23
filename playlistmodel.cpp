#include "playlistmodel.h"

PlaylistModel::PlaylistModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int PlaylistModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return m_rows.size();
}

bool PlaylistModel::updateRow(int row, const QString &name)
{
    if (row < 0 || row >= m_rows.size())
        return false;
    if (m_rows[row].name == name)
        return true;
    m_rows[row].name = name;
    const QModelIndex idx = index(row);
    emit dataChanged(idx, idx, {Qt::DisplayRole, Qt::EditRole});
    return true;
}

QVariant PlaylistModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size())
        return QVariant();

    const PlaylistRow &r = m_rows.at(index.row());
    switch (role) {
    case Qt::DisplayRole:
    case Qt::EditRole:
        return r.name;
    case PathRole:
        return r.path;
    case TypeRole:
        return r.type;
    default:
        return QVariant();
    }
}

Qt::ItemFlags PlaylistModel::flags(const QModelIndex &index) const
{
    if (!index.isValid())
        return Qt::NoItemFlags;
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
}

void PlaylistModel::setRows(QVector<PlaylistRow> rows)
{
    beginResetModel();
    m_rows = std::move(rows);
    endResetModel();
}

void PlaylistModel::clearRows()
{
    beginResetModel();
    m_rows.clear();
    endResetModel();
}

void PlaylistModel::appendRow(const PlaylistRow &row)
{
    beginInsertRows(QModelIndex(), m_rows.size(), m_rows.size());
    m_rows.append(row);
    endInsertRows();
}
