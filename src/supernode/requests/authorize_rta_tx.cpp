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

#include "supernode/requests/authorize_rta_tx.h"
#include "supernode/requestdefines.h"
#include "lib/graft/jsonrpc.h"
#include "supernode/requests/send_raw_tx.h"
#include "supernode/requests/multicast.h"
#include "supernode/requests/broadcast.h"
#include "rta/supernode.h"
#include <misc_log_ex.h>
#include <exception>

// logging
#undef MONERO_DEFAULT_LOG_CATEGORY
#define MONERO_DEFAULT_LOG_CATEGORY "supernode.authorizertatxrequest"

namespace {
    static const char * PATH_REQUEST =  "/cryptonode/authorize_rta_tx_request";
    static const char * PATH_RESPONSE = "/cryptonode/authorize_rta_tx_response";
    static const size_t RTA_VOTES_TO_REJECT =  1/*2*/; // TODO: 1 and 3 while testing
    static const size_t RTA_VOTES_TO_APPROVE = 4/*7*/;
}

namespace graft::supernode::request {

GRAFT_DEFINE_IO_STRUCT_INITED(SupernodeSignature,
                              (std::string, id_key, std::string()),
                              (std::string, result_signature, std::string()), // signarure for tx_id + result
                              (std::string, tx_signature, std::string())      // signature for tx_id only
                              );

GRAFT_DEFINE_IO_STRUCT_INITED(AuthorizeRtaTxRequestResponse,
                        (int, Result, STATUS_OK)
                       );


GRAFT_DEFINE_IO_STRUCT_INITED(AuthorizeRtaTxResponse,
                       (std::string, tx_id, std::string()),
                       (int, result, int(RTAAuthResult::Invalid)),
                       (SupernodeSignature, signature, SupernodeSignature())
                       );

GRAFT_DEFINE_IO_STRUCT_INITED(AuthorizeRtaTxResponseResponse,
                       (int, Result, STATUS_OK)
                       );

GRAFT_DEFINE_JSON_RPC_RESPONSE_RESULT(AuthorizeRtaTxRequestJsonRpcResponse, AuthorizeRtaTxRequestResponse);
GRAFT_DEFINE_JSON_RPC_RESPONSE_RESULT(AuthorizeRtaTxResponseJsonRpcResponse, AuthorizeRtaTxResponseResponse);

enum class RtaAuthRequestHandlerState : int {
    ClientRequest = 0, // incoming request from client
    ClientRequestAgain,
    CryptonodeReply   // cryptonode replied to milticast
};

enum class RtaAuthResponseHandlerState : int {
    // Multicast call from cryptonode auth rta auth response
    RtaAuthReply = 0,
    // we pushed tx to tx pool, next is to broadcast status,
    TransactionPushReply,
    // Status broadcast reply
    StatusBroadcastReply
};

struct RtaAuthResult
{
    std::vector<SupernodeSignature> approved;
    std::vector<SupernodeSignature> rejected;
    bool alreadyApproved(const std::string &id)
    {
        return contains(approved, id);
    }

    bool alreadyRejected(const std::string &id)
    {
        return contains(rejected, id);
    }

private:
    bool contains(const std::vector<SupernodeSignature> &v, const std::string &id)
    {
        return std::find_if(v.begin(), v.end(), [&](const SupernodeSignature &item) {
            return item.id_key == id;
        }) != v.end();
    }
};

// TODO: this function duplicates PendingTransaction::putRtaSignatures
void putRtaSignaturesToTx(cryptonote::transaction &tx, const std::vector<SupernodeSignature> &signatures, bool testnet)
{
#if 0
    std::vector<cryptonote::rta_signature> bin_signs;
    for (const auto &sign : signatures) {
        cryptonote::rta_signature bin_sign;
        if (!cryptonote::get_account_address_from_str(bin_sign.address, testnet, sign.id_key)) {
            LOG_ERROR("error parsing address from string: " << sign.id_key);
            continue;
        }
        epee::string_tools::hex_to_pod(sign.tx_signature, bin_sign.signature);
        bin_signs.push_back(bin_sign);
    }
    tx.put_rta_signatures(bin_signs);
#endif
}


/*!
 * \brief signAuthResponse - signs RTA auth result
 * \param arg
 * \param supernode
 * \return
 */
bool signAuthResponse(AuthorizeRtaTxResponse &arg, const SupernodePtr &supernode)
{
    crypto::signature sign;
    supernode->signMessage(arg.tx_id + ":" + std::to_string(arg.result), sign);
    arg.signature.result_signature = epee::string_tools::pod_to_hex(sign);
    crypto::hash tx_id;
    epee::string_tools::hex_to_pod(arg.tx_id, tx_id);
    supernode->signHash(tx_id, sign);
    arg.signature.tx_signature = epee::string_tools::pod_to_hex(sign);
    arg.signature.id_key = supernode->idKeyAsString();
    return true;
}

/*!
 * \brief validateAuthResponse - validates (checks) RTA auth result signed by supernode
 * \param arg
 * \param supernode
 * \return
 */
bool validateAuthResponse(const AuthorizeRtaTxResponse &arg, const SupernodePtr &supernode)
{
    crypto::signature sign_result;
    crypto::signature sign_tx_id;
    crypto::hash tx_id;
    if (!epee::string_tools::hex_to_pod(arg.signature.result_signature, sign_result)) {
        LOG_ERROR("Error parsing signature: " << arg.signature.result_signature);
        return false;
    }

    if (!epee::string_tools::hex_to_pod(arg.signature.tx_signature, sign_tx_id)) {
        LOG_ERROR("Error parsing signature: " << arg.signature.result_signature);
        return false;
    }

    if (!epee::string_tools::hex_to_pod(arg.tx_id, tx_id)) {
        LOG_ERROR("Error parsing tx_id: " << arg.tx_id);
        return false;
    }



    std::string msg = arg.tx_id + ":" + std::to_string(arg.result);
    crypto::public_key id_key;
    epee::string_tools::hex_to_pod(arg.signature.id_key, id_key);
    bool r1 = supernode->verifySignature(msg, id_key, sign_result);
    bool r2 = supernode->verifyHash(tx_id, id_key, sign_tx_id);
    return r1 && r2;
}

Status storeRequestAndReplyOk(const Router::vars_t& vars, const graft::Input& input,
                            graft::Context& ctx, graft::Output& output) noexcept
{
    // store input in local ctx.
    MDEBUG(__FUNCTION__ << " begin");
    ctx.local["request"] = input.data();

    // here request parsed for the logging purposes. normally we don't need to parse it here;
    MulticastRequestJsonRpc req;
    if (!input.get(req)) { // can't parse request
        return errorCustomError(std::string("failed to parse request: ")  + input.data(), ERROR_INVALID_REQUEST, output);
    }

    SupernodePtr supernode = ctx.global.get(CONTEXT_KEY_SUPERNODE, SupernodePtr());

    AuthorizeRtaTxRequest authReq;
    Input innerInput;
    innerInput.load(req.params.data);

    if (!innerInput.getT<serializer::JSON_B64>(authReq)) {
        return errorInvalidParams(output);
    }
    MDEBUG("incoming tx auth request from: " << req.params.sender_address
           << ", payment: " << authReq.payment_id);

    // reply ok to the client
    AuthorizeRtaTxRequestJsonRpcResponse out;
    out.result.Result = STATUS_OK;
    output.load(out);
    MDEBUG(__FUNCTION__ << " end");
    return Status::Again;
}

/*!
 * \brief handleTxAuthRequest - handles RTA auth request multicasted over auth sample. Handler either approves or rejects transaction
 * \param vars
 * \param input
 * \param ctx
 * \param output
 * \return
 */

Status handleTxAuthRequest(const Router::vars_t& vars, const graft::Input& /*input*/,
                            graft::Context& ctx, graft::Output& output) noexcept
{

    MDEBUG(__FUNCTION__ << " begin");
    assert(ctx.local.getLastStatus() == Status::Again);

    if (!ctx.local.hasKey("request")) {
        LOG_ERROR("Internal error. no input for 'again' status");
        return Status::Error;
    }

    graft::Input input;
    std::string body = ctx.local["request"];
    input.body = body;

    MulticastRequestJsonRpc req;
    if (!input.get(req)) { // can't parse request
        return errorCustomError(std::string("failed to parse request: ")  + input.data(), ERROR_INVALID_REQUEST, output);
    }

    SupernodePtr supernode = ctx.global.get(CONTEXT_KEY_SUPERNODE, SupernodePtr());

    AuthorizeRtaTxRequest authReq;
    Input innerInput;
    innerInput.load(req.params.data);

    if (!innerInput.getT<serializer::JSON_B64>(authReq)) {
        return errorInvalidParams(output);
    }

    // de-serialize transaction
    cryptonote::transaction tx;
    crypto::hash tx_hash, tx_prefix_hash;
    cryptonote::blobdata tx_blob;

    if (!epee::string_tools::parse_hexstr_to_binbuff(authReq.tx_hex, tx_blob)) {
        LOG_ERROR("Failed to parse hex tx: " << authReq.tx_hex);
        return errorInvalidTransaction(authReq.tx_hex, output);
    }

    if (!cryptonote::parse_and_validate_tx_from_blob(tx_blob, tx, tx_hash, tx_prefix_hash)) {
        LOG_ERROR("Failed to parse and validate tx from blob: " << authReq.tx_hex);
        return errorInvalidTransaction(authReq.tx_hex, output);
    }

    std::string tx_id_str = epee::string_tools::pod_to_hex(tx_hash);
    MDEBUG("incoming auth req for payment: " << authReq.payment_id
           << ", tx_id: " << tx_id_str);
    // check if we already processed this tx
    if (ctx.global.hasKey(tx_id_str + CONTEXT_KEY_TX_BY_TXID)) {
        LOG_ERROR("tx already processed: " << tx_id_str);
        return errorCustomError("tx already processed", ERROR_INVALID_PARAMS, output);
    }

    // store tx amount in global context
    MDEBUG("storing amount for payment: " << authReq.payment_id
           << ", tx_id: " << tx_id_str << ", amount: " << authReq.amount);
    ctx.global.set(tx_id_str + CONTEXT_KEY_AMOUNT_BY_TX_ID, authReq.amount, RTA_TX_TTL);
    // check if we have a fee assigned by sender wallet
    uint64 amount = 0;
    if (!supernode->getAmountFromTx(tx, amount)) {
        LOG_ERROR("can't parse supernode fee for tx: " << tx_id_str);
        return errorCustomError("can't parse supernode fee for tx", ERROR_INVALID_PARAMS, output);
    }

   // TODO: read payment id from transaction, map tx_id to payment_id
    MulticastRequestJsonRpc authResponseMulticast;
    authResponseMulticast.method = "multicast";
    authResponseMulticast.params.sender_address = supernode->idKeyAsString();
    authResponseMulticast.params.receiver_addresses = req.params.receiver_addresses;
    authResponseMulticast.params.callback_uri = PATH_RESPONSE;
    AuthorizeRtaTxResponse authResponse;
    authResponse.tx_id = tx_id_str;
    authResponse.result = static_cast<int>(amount > 0 && tx.type == cryptonote::transaction::tx_type_rta ?
                                             RTAAuthResult::Approved
                                           : RTAAuthResult::Rejected);
    signAuthResponse(authResponse, supernode);
    authResponse.signature.id_key  = supernode->idKeyAsString();

    // store tx
    ctx.global.set(authResponse.tx_id + CONTEXT_KEY_TX_BY_TXID, tx, RTA_TX_TTL);
    // TODO: remove it when payment id will be in tx.extra
    ctx.global.set(authResponse.tx_id + CONTEXT_KEY_PAYMENT_ID_BY_TXID, authReq.payment_id, RTA_TX_TTL);

    // store payment id in local ctx for the logging purposes
    ctx.local["payment_id"] = authReq.payment_id;

    Output innerOut;
    innerOut.loadT<serializer::JSON_B64>(authResponse);
    authResponseMulticast.params.data = innerOut.data();
    output.load(authResponseMulticast);
    output.path = "/json_rpc/rta";
    MDEBUG("payment: " << authReq.payment_id << ", validate result: " << authResponse.result);

    MDEBUG("multicasting: " << output.data());

    MDEBUG(__FUNCTION__ << " end");
    return Status::Forward;
}

/*!
 * \brief handleCryptonodeMulticastResponse - handles multicast response from cryptonode. Here we only interested in error checking,
 *                                            there's nothing in response except "ok"
 *
 * \param vars
 * \param input
 * \param ctx
 * \param output
 * \return
 */
Status handleCryptonodeMulticastStatus(const Router::vars_t& vars, const graft::Input& input,
                            graft::Context& ctx, graft::Output& output)
{

    // check cryptonode reply
    MDEBUG(__FUNCTION__ << " begin");
    MulticastResponseFromCryptonodeJsonRpc resp;

    JsonRpcErrorResponse error;
    if (!input.get(resp) || resp.error.code != 0 || resp.result.status != STATUS_OK) {
        return  errorInternalError("Error multicasting request", output);
    }
    std::string payment_id_local = ctx.local["payment_id"];
    MDEBUG("tx auth response multicast ask received for payment: " << payment_id_local);

    AuthorizeRtaTxRequestJsonRpcResponse out;
    out.result.Result = STATUS_OK;
    output.load(out);

    return Status::Ok;
}

/*!
 * \brief handleRtaAuthResponseMulticast - handles cryptonode/authorize_rta_tx_response call
 * \param vars
 * \param input
 * \param ctx
 * \param output
 * \return
 */
Status handleRtaAuthResponseMulticast(const Router::vars_t& vars, const graft::Input& input,
                            graft::Context& ctx, graft::Output& output)
{

    try {

        MulticastRequestJsonRpc req;
        MDEBUG(__FUNCTION__ << " begin");

        if (!input.get(req)) { // can't parse request
            LOG_ERROR("failed to parse request: " + input.data());
            return errorCustomError(std::string("failed to parse request: ")  + input.data(), ERROR_INVALID_REQUEST, output);
        }

        // TODO: check if our address is listed in "receiver_addresses"
        AuthorizeRtaTxResponse rtaAuthResp;
        Input innerIn;

        innerIn.load(req.params.data);

        if (!innerIn.getT<serializer::JSON_B64>(rtaAuthResp)) {
            LOG_ERROR("error deserialize rta auth response");
            return errorInvalidParams(output);
        }

        SupernodePtr supernode = ctx.global.get(CONTEXT_KEY_SUPERNODE, SupernodePtr());

        RTAAuthResult result = static_cast<RTAAuthResult>(rtaAuthResp.result);
        // sanity check
        if (result != RTAAuthResult::Approved && result != RTAAuthResult::Rejected) {
            LOG_ERROR("Invalid rta auth result: " << rtaAuthResp.result);
            return errorInvalidParams(output);
        }


        std::string ctx_payment_id_key = rtaAuthResp.tx_id + CONTEXT_KEY_PAYMENT_ID_BY_TXID;

        if (!ctx.global.hasKey(ctx_payment_id_key)) {
            LOG_ERROR("no payment_id for tx: " << rtaAuthResp.tx_id);
            return errorCustomError(std::string("unknown tx: ") + rtaAuthResp.tx_id, ERROR_INTERNAL_ERROR, output);
        }
        std::string payment_id = ctx.global.get(ctx_payment_id_key, std::string());
        MDEBUG("incoming tx auth response payment: " << payment_id
                     << ", tx_id: " << rtaAuthResp.tx_id
                     << ", from: " << rtaAuthResp.signature.id_key
                     << ", result: " << int(result));

        // store payment id for a logging purposes
        ctx.local["payment_id"] = payment_id;

        // validate signature
        bool signOk = validateAuthResponse(rtaAuthResp, supernode);
        if (!signOk) {
            std::string msg = "failed to validate signature for rta auth response";
            LOG_ERROR(msg);
            return errorCustomError(msg,
                                    ERROR_RTA_SIGNATURE_FAILED,
                                    output);
        }
        // stop handling it if we already processed response
        RtaAuthResult authResult;
        std::string ctx_tx_to_auth_resp = rtaAuthResp.tx_id + CONTEXT_KEY_AUTH_RESULT_BY_TXID;
        if (ctx.global.hasKey(ctx_tx_to_auth_resp)) {
            authResult = ctx.global.get(ctx_tx_to_auth_resp, authResult);
        }

        if (authResult.alreadyApproved(rtaAuthResp.signature.id_key)
                || authResult.alreadyRejected(rtaAuthResp.signature.id_key)) {
            return errorCustomError(std::string("supernode: ") + rtaAuthResp.signature.id_key + " already processed",
                                    ERROR_ADDRESS_INVALID, output);
        }

        if (result == RTAAuthResult::Approved) {
            authResult.approved.push_back(rtaAuthResp.signature);
        } else {
            authResult.rejected.push_back(rtaAuthResp.signature);
        }

        MDEBUG("rta result accepted from " << rtaAuthResp.signature.id_key
               << ", payment: " << payment_id);

        // store result in context
        ctx.global.set(ctx_tx_to_auth_resp, authResult, RTA_TX_TTL);
        if (!ctx.global.hasKey(rtaAuthResp.tx_id + CONTEXT_KEY_AMOUNT_BY_TX_ID)) {
            std::string msg = std::string("no amount found for tx id: ") + rtaAuthResp.tx_id;
            LOG_ERROR(msg);
            return errorCustomError(msg, ERROR_INTERNAL_ERROR, output);
        }

        uint64_t tx_amount = ctx.global.get(rtaAuthResp.tx_id + CONTEXT_KEY_AMOUNT_BY_TX_ID, uint64_t(0));

        size_t rta_votes_to_approve = tx_amount / COIN > 100 ? 4 : 2;

        MDEBUG("approved votes: " << authResult.approved.size()
               << "/" << rta_votes_to_approve
               << ", rejected votes: " << authResult.rejected.size()
               << ", payment: " << payment_id);


        if (!ctx.global.hasKey(rtaAuthResp.tx_id + CONTEXT_KEY_TX_BY_TXID)) {
            std::string msg = std::string("rta auth response processed but no tx found for tx id: ") + rtaAuthResp.tx_id;
            LOG_ERROR(msg);
            return errorCustomError(msg, ERROR_INTERNAL_ERROR, output);
        }

        if (authResult.rejected.size() >= RTA_VOTES_TO_REJECT) {
            MDEBUG("payment: " << payment_id
                   << ", tx_id: " << rtaAuthResp.tx_id
                   << " rejected by auth sample, updating status");

            // tx rejected by auth sample, broadcast status;
            ctx.global[__FUNCTION__] = RtaAuthResponseHandlerState::StatusBroadcastReply;
            ctx.global.set(payment_id + CONTEXT_KEY_STATUS, static_cast<int> (RTAStatus::Fail), RTA_TX_TTL);
            buildBroadcastSaleStatusOutput(payment_id, static_cast<int> (RTAStatus::Fail), supernode, output);
            return Status::Forward;
        } else if (authResult.approved.size() >= rta_votes_to_approve) {
            MDEBUG("payment: " << payment_id
                   << ", tx_id: " << rtaAuthResp.tx_id
                   << " approved by auth sample, pushing tx to pool");

            SendRawTxRequest req;
            // store tx_id in local context so we can use it when broadcasting status
            ctx.local[CONTEXT_TX_ID] = rtaAuthResp.tx_id;
            cryptonote::transaction tx = ctx.global.get(rtaAuthResp.tx_id + CONTEXT_KEY_TX_BY_TXID, cryptonote::transaction());
            putRtaSignaturesToTx(tx, authResult.approved, supernode->testnet());
            createSendRawTxRequest(tx, req);
#if 0
            // kept for future debugging
            {
                MDEBUG("sending tx to cryptonode:  " << req.tx_as_hex);
                MDEBUG("  rta signatures: ");
                std::string buf;
                buf += "\n";
                for (const auto & rta_sign:  tx.rta_signatures) {
                    buf += string("      address: ") + cryptonote::get_account_address_as_str(true, rta_sign.address) + "\n";
                    buf += string("      signature: ") + epee::string_tools::pod_to_hex(rta_sign.signature) + "\n";
                }
                MDEBUG(buf);
            }
#endif

            // call cryptonode
            output.load(req);
            output.path = "/sendrawtransaction";
            ctx.local[__FUNCTION__] = RtaAuthResponseHandlerState::TransactionPushReply;

            return Status::Forward;
        } else {
            MDEBUG("not enough votes for approval/reject for payment: " << payment_id
                   << ", keep waiting for other votes");
            AuthorizeRtaTxResponseJsonRpcResponse out;
            out.result.Result = STATUS_OK;
            output.load(out);
            MDEBUG(__FUNCTION__ << " end");
            return Status::Ok;
        }


    } catch (const std::exception &e) {
        LOG_ERROR("std::exception  catched: " << e.what());
        return errorInternalError(std::string("exception in cryptonode/authorize_rta_tx_response handler: ") +  e.what(),
                                  output);
    } catch (...) {
        LOG_ERROR("unhandled exception");
        return errorInternalError(std::string("unknown exception in cryptonode/authorize_rta_tx_response handler"),
                                  output);
    }
}


// handles "/sendrawtransaction" response
Status handleCryptonodeTxPushResponse(const Router::vars_t& vars, const graft::Input& input,
                               graft::Context& ctx, graft::Output& output)
{

    MDEBUG(__FUNCTION__ << " begin for task: " << boost::uuids::to_string(ctx.getId()));
    std::string payment_id_local = ctx.local["payment_id"];
    MDEBUG("processing sendrawtransaction reply for payment: " << payment_id_local);

    SendRawTxResponse resp;
    // check if we have tx_id in local context
    std::string tx_id = ctx.local[CONTEXT_TX_ID];

    if (tx_id.empty()) {
        LOG_ERROR("internal erorr, tx_id key not found in local context");
        abort();
    }

    // obtain payment id for given tx_id
    std::string payment_id = ctx.global.get(tx_id + CONTEXT_KEY_PAYMENT_ID_BY_TXID, std::string());
    if (payment_id.empty()) {
        LOG_ERROR("Internal error, payment id not found for tx id: " << tx_id);
    }

    RTAStatus status = static_cast<RTAStatus>(ctx.global.get(payment_id + CONTEXT_KEY_STATUS, int(RTAStatus::None)));
    if (status == RTAStatus::None) {
        LOG_ERROR("can't find status for payment_id: " << payment_id);
        return errorInvalidParams(output);
    }


    if (!input.get(resp)) {
        LOG_ERROR("Failed to parse input: " << input.data());
        return errorInvalidParams(output);
    }

    if (status == RTAStatus::Success
            || status == RTAStatus::Fail
            || status == RTAStatus::RejectedByPOS
            || status == RTAStatus::RejectedByWallet) {
        MWARNING("payment: " << payment_id << ", most likely already processed,  status: " << int(status));
        return sendOkResponseToCryptonode(output);
    }

    if (resp.status != "OK") {
        // check for double spend
        if (resp.double_spend) { // specific cast, we can get cryptonode's /sendrawtransaction response before status broadcast,
                                 // just ignore it for now
            LOG_ERROR("double spend for payment: " << payment_id << ", tx: " << tx_id);
            return sendOkResponseToCryptonode(output);
        }
        status = RTAStatus::Fail;
        // LOG_ERROR("failed to put tx to pool: " << tx_id << ", reason: " << resp.reason);
        LOG_ERROR("failed to put tx to pool: " << tx_id << ", input: " << input.data());
    } else {
        status = RTAStatus::Success;
    }

    SupernodePtr supernode = ctx.global.get(CONTEXT_KEY_SUPERNODE, SupernodePtr());

    MDEBUG("broadcasting status for payment id: " << payment_id << ", status : " << int(status));
    buildBroadcastSaleStatusOutput(payment_id, int(status), supernode, output);
    ctx.local[__FUNCTION__] = RtaAuthResponseHandlerState::StatusBroadcastReply;
    MDEBUG(__FUNCTION__ << " end");
    return Status::Forward;
}

// handles status broadcast response, pass "ok" to caller (cryptonode)
Status handleStatusBroadcastResponse(const Router::vars_t& vars, const graft::Input& input,
                                     graft::Context& ctx, graft::Output& output)
{
    // TODO: check if cryptonode broadcasted status
    MDEBUG(__FUNCTION__ << " begin");
    std::string payment_id_local = ctx.local["payment_id"];
    MDEBUG("received status broadcasting result for payment: " << payment_id_local);
    BroadcastResponseFromCryptonodeJsonRpc in;
    JsonRpcErrorResponse error;
    if (!input.get(in) || in.error.code != 0 || in.result.status != STATUS_OK) {
        return errorCustomError("Error broadcasting status", ERROR_INTERNAL_ERROR, output);
    }


    // most likely cryptonode doesn't really care what we reply here
    AuthorizeRtaTxResponseJsonRpcResponse outResponse;
    outResponse.result.Result = STATUS_OK;
    output.load(outResponse);
    MDEBUG(__FUNCTION__ << " end");
    return Status::Ok;
}



/*!
 * \brief authorizeRtaTxRequestHandler - called by supernode as multicast request. handles rta authorization request
 * \param vars
 * \param input
 * \param ctx
 * \param output
 * \return
 */
Status authorizeRtaTxRequestHandler(const Router::vars_t& vars, const graft::Input& input,
                                 graft::Context& ctx, graft::Output& output) noexcept
{
    try {
        RtaAuthRequestHandlerState state = ctx.local.hasKey(__FUNCTION__) ? ctx.local[__FUNCTION__] : RtaAuthRequestHandlerState::ClientRequest;
        switch (state) {
        case RtaAuthRequestHandlerState::ClientRequest:
            ctx.local[__FUNCTION__] = RtaAuthRequestHandlerState::ClientRequestAgain;
            return storeRequestAndReplyOk(vars, input, ctx, output);
        case RtaAuthRequestHandlerState::ClientRequestAgain:
            ctx.local[__FUNCTION__] = RtaAuthRequestHandlerState::CryptonodeReply;
            return handleTxAuthRequest(vars, input, ctx, output);
        case RtaAuthRequestHandlerState::CryptonodeReply:
            MDEBUG("cyptonode reply, payload: " << input.data());
            return handleCryptonodeMulticastStatus(vars, input, ctx, output);
        default: // internal error
            return errorInternalError(std::string("authorize_rta_tx_request: unhandled state: ") + std::to_string(int(state)),
                                      output);
        };
    } catch (const std::exception &e) {
        LOG_ERROR(__FUNCTION__ << " exception thrown: " << e.what());
    } catch (...) {
        LOG_ERROR(__FUNCTION__ << " unknown exception thrown");
    }
    return errorInternalError("exception thrown", output);

}

/*!
 * \brief authorizeRtaTxResponseHandler - handles supernode's RTA auth response multicasted over auth sample
 * \param vars
 * \param input
 * \param ctx
 * \param output
 * \return
 */
Status authorizeRtaTxResponseHandler(const Router::vars_t& vars, const graft::Input& input,
                                 graft::Context& ctx, graft::Output& output)
{

    try {
        RtaAuthResponseHandlerState state = ctx.local.hasKey(__FUNCTION__) ? ctx.local[__FUNCTION__] : RtaAuthResponseHandlerState::RtaAuthReply;
        MDEBUG(__FUNCTION__ << " state: " << int(state) << ", status: "<< (int) ctx.local.getLastStatus() << ", task id: " << boost::uuids::to_string(ctx.getId()));

        switch (state) {
        // actually not a reply, just incoming multicast. same as "called by client" and client is cryptonode here
        case RtaAuthResponseHandlerState::RtaAuthReply:
            ctx.local[__FUNCTION__] = RtaAuthResponseHandlerState::TransactionPushReply;
            return handleRtaAuthResponseMulticast(vars, input, ctx, output);

        case RtaAuthResponseHandlerState::TransactionPushReply:
            ctx.local[__FUNCTION__] = RtaAuthResponseHandlerState::StatusBroadcastReply;
            return handleCryptonodeTxPushResponse(vars, input, ctx, output);

        case RtaAuthResponseHandlerState::StatusBroadcastReply:
            return handleStatusBroadcastResponse(vars, input, ctx, output);
        default:
            LOG_ERROR("Internal error, unexpected state: " << (int)state);
            abort();
        };
    } catch (const std::exception &e) {
        LOG_ERROR(__FUNCTION__ << " exception thrown: " << e.what());
    } catch (...) {
        LOG_ERROR(__FUNCTION__ << " unknown exception thrown");
    }
    return errorInternalError("exception thrown", output);


}

void registerAuthorizeRtaTxRequests(graft::Router &router)
{
    Router::Handler3 request_handler(nullptr, authorizeRtaTxRequestHandler, nullptr);
    Router::Handler3 response_handler(nullptr, authorizeRtaTxResponseHandler, nullptr);
    router.addRoute(PATH_REQUEST, METHOD_POST, request_handler);
    LOG_PRINT_L1("route " << PATH_REQUEST << " registered");
    router.addRoute(PATH_RESPONSE, METHOD_POST, response_handler);
    LOG_PRINT_L1("route " << PATH_RESPONSE << " registered");
}

}

