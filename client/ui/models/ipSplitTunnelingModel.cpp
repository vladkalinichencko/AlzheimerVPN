#include "ipSplitTunnelingModel.h"
#include "core/controllers/ipSplitTunnelingController.h"

IpSplitTunnelingModel::IpSplitTunnelingModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

void IpSplitTunnelingModel::setController(IpSplitTunnelingController *controller)
{
    m_controller = controller;
    connect(controller, &IpSplitTunnelingController::siteCheckChanged, this, [this](const QString &host) {
        for (int row = 0; row < m_sites.size(); ++row) {
            if (m_sites[row].first == host) {
                emit dataChanged(index(row), index(row));
                break;
            }
        }
    });
}

int IpSplitTunnelingModel::rowCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent)
    return m_sites.size();
}

QVariant IpSplitTunnelingModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(rowCount()))
        return QVariant();

    const auto &site = m_sites.at(index.row());
    const auto check = m_controller ? m_controller->siteCheck(site.first)
                                   : IpSplitTunnelingController::SiteCheck{};
    using State = IpSplitTunnelingController::CheckState;
    switch (role) {
    case UrlRole: {
        return m_sites.at(index.row()).first;
        break;
    }
    case IpRole: {
        return m_sites.at(index.row()).second.join(", ");
        break;
    }
    case CheckFailedRole:
        return check.state == State::Failed || (check.state == State::Unchecked && site.second.isEmpty());
    case CheckSucceededRole:
        return check.state == State::Address || check.state == State::Resolved
            || (check.state != State::Failed && !site.second.isEmpty());
    default: {
        return true;
    }
    }

    return QVariant();
}

void IpSplitTunnelingModel::updateModel(const QVector<QPair<QString, QStringList>> &sites)
{
    beginResetModel();
    m_sites = sites;
    endResetModel();
}

QHash<int, QByteArray> IpSplitTunnelingModel::roleNames() const
{
    QHash<int, QByteArray> roles;
    roles[UrlRole] = "url";
    roles[IpRole] = "ip";
    roles[CheckFailedRole] = "checkFailed";
    roles[CheckSucceededRole] = "checkSucceeded";
    return roles;
}
