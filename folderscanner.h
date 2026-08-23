#ifndef FOLDERSCANNER_H
#define FOLDERSCANNER_H

#include <QElapsedTimer>
#include <QThread>
#include <QString>
#include <QStringList>
#include <QDir>
#include "mainwindow.h"

// Forward declaration of PlaylistTreeNode from mainwindow.h
// Wait, PlaylistTreeNode is defined in mainwindow.h.
// We can just include mainwindow.h.

class FolderScanner : public QThread
{
    Q_OBJECT
public:
    FolderScanner(const QString& folderPath, QObject* parent = nullptr);
    ~FolderScanner();

    // The root node containing the scanned structure
    PlaylistTreeNode* getResultNode() const { return m_resultNode; }
    QString getFolderPath() const { return m_folderPath; }

    // The extensions this scanner picks up. Shared with the counting pass so
    // the denominator cannot drift from what is actually added.
    static const QStringList& playableFilters();

signals:
    void scanFinished(FolderScanner* scanner);

    // How far the scan has got, so the placeholder row can say so. Emitted at
    // most ten times a second - a folder of 114,727 files would otherwise put
    // that many queued events on the GUI thread and cost more than the scan.
    //
    // `total` is 0 while the counting pass is still running; a scan of a large
    // tree spends a second or two there and would otherwise show a number with
    // nothing to compare it against.
    void progress(int done, int total);

protected:
    void run() override;

private:
    QString m_folderPath;
    PlaylistTreeNode* m_resultNode;

    int m_done = 0;
    int m_total = 0;
    QElapsedTimer m_sinceLastReport;

    int countPlayableFiles(const QString& folderPath) const;
    void reportProgress(bool force);

    void addFolderStructureToNode(PlaylistTreeNode* parentNode, const QString& folderPath);
    bool isOplFile(const QString& filePath);
};

#endif // FOLDERSCANNER_H
