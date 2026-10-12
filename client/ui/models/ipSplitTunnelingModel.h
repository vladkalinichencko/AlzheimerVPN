#ifndef IPSPLITTUNNELINGMODEL_H
#define IPSPLITTUNNELINGMODEL_H

#include <QAbstractListModel>
#include <QVector>
#include <QPair>
#include <QStringList>

class IpSplitTunnelingController;

class IpSplitTunnelingModel : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Roles {
        UrlRole = Qt::UserRole + 1,
        IpRole,
        CheckFailedRole,
        CheckSucceededRole
    };

    explicit IpSplitTunnelingModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;

    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    void setController(IpSplitTunnelingController *controller);

public slots:
    void updateModel(const QVector<QPair<QString, QStringList>> &sites);

protected:
    QHash<int, QByteArray> roleNames() const override;

private:
    QVector<QPair<QString, QStringList>> m_sites;
    IpSplitTunnelingController *m_controller = nullptr;
};

#endif // IPSPLITTUNNELINGMODEL_H
