#ifndef OPLSTEREODIALOG_H
#define OPLSTEREODIALOG_H

#include <QDialog>
#include <QListWidget>

class OplStereoDialog : public QDialog {
    Q_OBJECT
public:
    explicit OplStereoDialog(int currentMode, QWidget* parent = nullptr);
    ~OplStereoDialog();

    int getSelectedMode() const { return m_selectedMode; }

protected:
    void keyPressEvent(QKeyEvent* event) override;
    bool eventFilter(QObject* obj, QEvent* event) override;

private:
    void setupUi();
    void selectModeAndAccept(int mode);

    QListWidget* m_listWidget;
    int m_selectedMode;
};

#endif // OPLSTEREODIALOG_H
