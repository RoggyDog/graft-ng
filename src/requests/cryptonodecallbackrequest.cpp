// Copyright (c) 2018, The Graft Project
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

#include "cryptonodecallbackrequest.h"
#include "requestdefines.h"
#include "sendrawtxrequest.h"
#include <misc_log_ex.h>


namespace {
    static const char * PATH = "/cryptonode_callback";

}



namespace graft {

GRAFT_DEFINE_IO_STRUCT_INITED(TxToSendCallbackRequest,
                              (crypto::public_key, auth_supernode_addr, crypto::public_key()),
                              (crypto::hash, hash, crypto::hash()),
                              (crypto::signature, signature, crypto::signature()),
                              (std::string, tx_as_blob, "")
                              );

GRAFT_DEFINE_IO_STRUCT_INITED(TxToSendCallbackResponse,
                              (std::string, status, "")
                              );

GRAFT_DEFINE_JSON_RPC_REQUEST(TxToSendCallbackJsonRpcRequest, TxToSendCallbackRequest);
GRAFT_DEFINE_JSON_RPC_RESPONSE(TxToSendCallbackJsonRpcResponse, TxToSendCallbackResponse);
GRAFT_DEFINE_JSON_RPC_RESPONSE_RESULT(TxToSendCallbackJsonRpcResponseResult, TxToSendCallbackResponse);


/**
 * @brief cryptonodeCallbacksHandler - function handing json-rpc callbacks from cryptonode
 * @param vars
 * @param input
 * @param ctx
 * @param output
 * @return
 */
Status cryptonodeCallbacksHandler(const Router::vars_t& vars, const graft::Input& input,
                                 graft::Context& ctx, graft::Output& output)
{
    LOG_PRINT_L2(PATH << "called with payload: " << input.toString());

}

void registerCryptonodeCallbacksRequest(graft::Router &router)
{
    Router::Handler3 h3(nullptr, cryptonodeCallbacksHandler, nullptr);

    router.addRoute(PATH, METHOD_POST, h3);
    LOG_PRINT_L2("route " << PATH << " registered");
}


}
