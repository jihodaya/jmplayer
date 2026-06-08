#ifndef FOLDERSCANNER_H
#define FOLDERSCANNER_H

#include <QThread>
#include <QString>
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

signals:
    void scanFinished(FolderScanner* scanner);

protected:
    void run() override;

private:
    QString m_folderPath;
    PlaylistTreeNode* m_resultNode;

    void addFolderStructureToNode(PlaylistTreeNode* parentNode, const QString& folderPath);
    bool isOplFile(const QString& filePath);
};

#endif // FOLDERSCANNER_H
