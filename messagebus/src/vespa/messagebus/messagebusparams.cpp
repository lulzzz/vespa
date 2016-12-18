// Copyright 2016 Yahoo Inc. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.
#include "messagebus.h"
#include <vespa/messagebus/routing/retrytransienterrorspolicy.h>

namespace mbus {

MessageBusParams::MessageBusParams() :
    _protocols(),
    _retryPolicy(new RetryTransientErrorsPolicy()),
    _maxPendingCount(1024),
    _maxPendingSize(128 * 1024 * 1024)
{
    // empty
}

uint32_t
MessageBusParams::getNumProtocols() const
{
    return _protocols.size();
}

IProtocol::SP
MessageBusParams::getProtocol(uint32_t i) const
{
    return _protocols[i];
}

MessageBusParams &
MessageBusParams::addProtocol(IProtocol::SP protocol)
{
    _protocols.push_back(protocol);
    return *this;
}

} // namespace mbus
