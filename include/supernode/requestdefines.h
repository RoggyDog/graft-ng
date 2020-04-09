
#pragma once

#include "lib/graft/requests/requestdefines.h"

//RTA DAPI Errors
#define ERROR_AMOUNT_INVALID                -32050
#define ERROR_PAYMENT_ID_INVALID            -32051
#define ERROR_ADDRESS_INVALID               -32052
#define ERROR_SALE_REQUEST_FAILED           -32060
#define ERROR_RTA_COMPLETED                 -32070
#define ERROR_RTA_FAILED                    -32071
#define ERROR_RTA_SIGNATURE_FAILED          -32080
#define ERROR_TRANSACTION_INVALID           -32090
#define ERROR_INVALID_SIGNATURE             -32100

static const std::string MESSAGE_AMOUNT_INVALID("Amount is invalid.");
static const std::string MESSAGE_PAYMENT_ID_INVALID("Payment ID is invalid.");
static const std::string MESSAGE_SALE_REQUEST_FAILED("Sale request is failed.");
static const std::string MESSAGE_RTA_COMPLETED("Payment is already completed.");
static const std::string MESSAGE_RTA_FAILED("Payment is already failed.");
static const std::string MESSAGE_ADDRESS_INVALID("Address in invalid.");
static const std::string MESSAGE_RTA_CANT_BUILD_AUTH_SAMPLE("Can't build auth sample.");
static const std::string MESSAGE_INVALID_TRANSACTION("Can't parse transaction");
static const std::string MESSAGE_INVALID_SIGNATURE("Signature check failed.");
static const std::string MESSAGE_RTA_INVALID_AUTH_SAMLE("Invalid auth sample");

//Context Keys
static const std::string CONTEXT_KEY_PAYMENT_DATA(":paymentdata"); // key to get/store encrypted payment data + keys
static const std::string CONTEXT_KEY_PAYMENT_DATA_PENDING(":paymentdata_pending"); // key to store flag if payment data already requested from remote supernode
static const std::string CONTEXT_KEY_SALE_DETAILS(":saledetails"); // TODO: remove it
static const std::string CONTEXT_KEY_SALE(":sale");
static const std::string CONTEXT_KEY_STATUS(":status");
static const std::string CONTEXT_KEY_PAY(":pay");
static const std::string CONTEXT_KEY_SUPERNODE("supernode");
static const std::string CONTEXT_KEY_FULLSUPERNODELIST("fsl");
static const std::string CONTEXT_KEY_TX(":tx");
// key to maintain auth responses from supernodes for given tx id
static const std::string CONTEXT_KEY_AUTH_RESULT_BY_TXID(":tx_id_to_auth_resp");
// key to map tx_id -> payment_id
static const std::string CONTEXT_KEY_PAYMENT_ID_BY_TXID(":tx_id_to_payment_id");
// key to map tx_id -> tx
static const std::string CONTEXT_KEY_TX_BY_TXID(":tx_id_to_tx");
// key to store tx id in local context
static const std::string CONTEXT_TX_ID("tx_id");
// key to map tx_id -> amount
static const std::string CONTEXT_KEY_AMOUNT_BY_TX_ID(":tx_id_to_amount");

// key to store sale_details response coming from callback
static const std::string CONTEXT_SALE_DETAILS_RESULT(":sale_details_result");

static const double AUTHSAMPLE_FEE_PERCENTAGE = 0.5;

static const std::string CONTEXT_KEY_CONFIG_OPTS(":config-opts");
static const std::string CONTEXT_KEY_RUNTIME_SYS_INFO(":runtime-sys-info");

// key to store if we already processed rta tx
static const std::string CONTEXT_KEY_RTA_TX_STATE(":rta-tx-state");

// key to store tx with signatures
static const std::string CONTEXT_KEY_RTA_VOTING_TX(":rta-voting-tx");

namespace graft {

static const std::chrono::seconds RTA_TX_TTL(60);


class Context;

Status errorInvalidPaymentID(Output &output);
Status errorInvalidAmount(Output &output);
Status errorInvalidAddress(Output &output);
Status errorBuildAuthSample(Output &output);
Status errorInvalidTransaction(const std::string &tx_data, Output &output);
Status errorInternalError(const std::string &message, Output &output);
Status errorCustomError(const std::string &message, int code, Output &output);
Status errorInvalidSignature(Output &output);
Status sendOkResponseToCryptonode(Output &output);
Status sendAgainResponseToCryptonode(Output &output);


bool errorFinishedPayment(int status, Output &output);
/*!
 * \brief cleanPaySaleData - cleans any data associated with given payment id
 * \param payment_id       - payment-id
 * \param ctx              - context
 */
void cleanPaySaleData(const std::string &payment_id, graft::Context &ctx);


enum class RTAStatus : int
{
    None = 0,
    InProgress = 2,    // non-continous (compatibility issues)
    Success = 3,
    FailRejectedByPOS, // rejected by PoS
    FailZeroFee,       // rejected by auth sample due low or zero fee
    FailDoubleSpend,   // rejected by auth sample due double spend
    FailTimedOut,      // timed out
    FailTxRejected     // tx rejected by cryptonode

};


/*!
 * \brief isFiniteRtaStatus - checks if given rta status is finite, i.e. no transition possible for given status
 * \param status            - input status
 * \return                  - true if status finite
 */
bool isFiniteRtaStatus(RTAStatus status);

/*!
 * \brief The RTAAuthResult enum - result of RTA TX verification
 */
enum class RTAAuthResult : int
{
    Approved = 0,
    Rejected = 1,
    Invalid  = 3
};

struct SaleData
{
    SaleData(const std::string &address, const uint64_t &blockNumber,
             const uint64_t &amount)
        : Address(address)
        ,BlockNumber(blockNumber)
        ,Amount(amount)
    {
    }
    SaleData() = default;

    std::string Address;
    uint64_t BlockNumber;
    uint64_t Amount;
};

struct PayData
{
    PayData(const std::string &address, const uint64_t &blockNumber,
            const uint64_t &amount)
        : Address(address)
        ,BlockNumber(blockNumber)
        ,Amount(amount)
    {
    }

    std::string Address;
    uint64_t BlockNumber;
    uint64_t Amount;
};

/*!
 * \brief broadcastSaleStatus -  sale (pay) status helper
 * \return
 */
void buildBroadcastSaleStatusOutput(const std::string &payment_id, int status, const SupernodePtr &supernode, Output &output);


template<typename Response>
Status storeRequestAndReplyOk(const Router::vars_t& vars, const graft::Input& input,
                            graft::Context& ctx, graft::Output& output) noexcept
{
    // store input in local ctx.
    ctx.local["request"] = input.data();

    Response response;
    response.result.Status = STATUS_OK;
    output.load(response);
    return Status::Again;
}

template <typename T>
std::string to_json_str(const T &object)
{
    graft::Output out;
    out.load(object);
    return out.data();
}

template <typename T>
bool from_json_str(const std::string &buf, T &object)
{
    graft::Input in;
    in.load(buf);
    return in.get(object);
}


} //namespace graft

