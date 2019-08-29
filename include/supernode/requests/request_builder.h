// Copyright (c) 2019, The Graft Project
//
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without modification, are
// permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this list of
//    conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice, this list
//    of conditions and the following disclaimer in the documentation and/or other
//    materials provided with the distribution.
//
// 3. Neither the name of the copyright holder nor the names of its contributors may be
//    used to endorse or promote products derived from this software without specific
//    prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
// THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
// STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
// THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#pragma once

#include "lib/graft/jsonrpc.h"
#include "lib/graft/inout.h"
#include "supernode/requestdefines.h"

#include "broadcast.h"
#include "multicast.h"
#include "unicast.h"

#include <vector>
#include <string>

namespace graft::supernode::request {
// move to output builder

template <typename Msg>
struct MethodName
{
};

template <>
struct MethodName<BroadcastRequestJsonRpc>
{
    std::string name { "broadcast" };
};

template <>
struct MethodName<MulticastRequestJsonRpc>
{
    std::string name { "multicast" };
};

template <>
struct MethodName<UnicastRequestJsonRpc>
{
    std::string name { "unicast" };
};

template <typename MsgType>
void buildMessageCommon(MsgType &req, SupernodePtr sn, const std::string &data, const std::string &callback_uri)
{

    req.params.sender_address = sn->idKeyAsString();
    req.params.data = data;
    req.params.callback_uri = callback_uri;
    req.method = MethodName<MsgType>().name;

}

template <typename MsgType, typename DestinationType>
void buildOutput(Output &output, MsgType &msg, SupernodePtr sn, const std::string &data, const std::string &callback_uri,
                 const DestinationType &destination, const std::string &path)
{
    buildMessageCommon(msg, sn, data, callback_uri);
    msg.params.receiver_addresses = destination;
    output.load(msg);
    output.path = path;
}

template <>
void buildOutput<UnicastRequestJsonRpc, std::string>(Output &output, UnicastRequestJsonRpc &msg,  SupernodePtr sn,
                                                     const std::string &data, const std::string &callback_uri,
                                                     const std::string &destination, const std::string &path)
{
    buildMessageCommon(msg, sn, data, callback_uri);
    msg.params.receiver_address = destination;
    output.load(msg);
    output.path = path;
}


}



