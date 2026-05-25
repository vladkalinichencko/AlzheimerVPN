#include "awgProtocol.h"

Awg::Awg(const QJsonObject &configuration, QObject *parent)
    : WireguardProtocol(amnezia::Proto::Awg, configuration, parent)
{
}

Awg::~Awg()
{
}
