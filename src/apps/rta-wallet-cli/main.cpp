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

#include "common.h"
#include "misc_log_ex.h"
#include "cryptonote_basic/cryptonote_format_utils.h"
#include "cryptonote_core/cryptonote_core.h"
#include "serialization/binary_utils.h" // dump_binary(), parse_binary()
#include "serialization/json_utils.h" // dump_json()
#include "include_base_utils.h"
#include "wallet/wallet2.h"

#include "rta/requests/pay.h"
#include "rta/requests/getpaymentdata.h"
#include "rta/requests/getsupernodeinfo.h"
#include "rta/requests/common.h"
#include "supernode/requestdefines.h"

#include "utils/cryptmsg.h" // one-to-many message cryptography
#include "lib/graft/serialize.h"

#include <atomic>
#include <cstdio>
#include <algorithm>
#include <string>
#include <fstream>
#include <chrono>

#include <boost/filesystem.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/uuid/uuid.hpp>
#include <unistd.h>


#undef MONERO_DEFAULT_LOG_CATEGORY
#define MONERO_DEFAULT_LOG_CATEGORY "rta-wallet-cli"

using namespace graft::supernode::request;
using namespace graft::rta::apps;
namespace po = boost::program_options;
using namespace cryptonote;
using namespace epee;
using namespace std;
using namespace graft::supernode;


namespace  {
    bool append_key_to_rta_hdr(cryptonote::rta_header &rta_hdr, const std::string &key)
    {
        crypto::public_key K;
        if (!epee::string_tools::hex_to_pod(key, K))  {
            MERROR("Failed to parse key from: " << key);
            return false;
        }
        rta_hdr.keys.push_back(K);
        return true;
    }

}

//=======================================================
class Wallet
{
public:
    Wallet(const std::string &qrcode_file, const std::string &supernode_address,
           const std::string &cryptonode_address, // TODO: check if supernode able to proxy wallet rpc
           uint64_t sale_timeout, const std::string &wallet_path, const std::string &wallet_password)
        : m_qrcode_file(qrcode_file)
        , m_sale_timeout(std::chrono::milliseconds(sale_timeout))
        , m_wallet_file(wallet_path)
        , m_wallet(cryptonote::TESTNET)

    {
        boost::optional<epee::net_utils::http::login> login{};
        m_http_client.set_server(supernode_address, login);

        bool keys_file_exists;
        bool wallet_file_exists;

        tools::wallet2::wallet_exists(wallet_path, keys_file_exists, wallet_file_exists);

        MDEBUG("keys_file_exists: " << boolalpha << keys_file_exists << noboolalpha
                     << "  wallet_file_exists: " << boolalpha << wallet_file_exists << noboolalpha);

        // existing wallet, open it
        if (keys_file_exists) {
            m_wallet.load(wallet_path, wallet_password);
        // new wallet, generating it
        } else {
            crypto::secret_key recovery_val, secret_key;
            recovery_val = m_wallet.generate(wallet_path, wallet_password, secret_key, false, false);
        }
        m_wallet.init(cryptonode_address);
        m_wallet.store();
        MINFO("wallet opened: " << m_wallet.get_address_as_str());
        // open wallet;
    }

    bool readPaymentDetails()
    {
        std::string json;
        bool r = epee::file_io_utils::load_file_to_string(m_qrcode_file, json);
        if (!r) {
            MERROR("Failed to reaf file: " << m_qrcode_file);
            return r;
        }
        graft::Input in; in.load(json);

        if (!in.get(m_paymentDetails)) {
            MERROR("Failed to parse json: " << json);
            return r;
        }

        if (!epee::string_tools::hex_to_pod(m_paymentDetails.key, m_decryption_key)) {
            MERROR("Failed to deserialize key from: " << m_paymentDetails.key);
            return false;
        }

        return true;

    }

    bool getPaymentData()
    {
        PaymentDataRequest req;
        req.PaymentID   = m_paymentDetails.paymentId;
        req.BlockHeight = m_paymentDetails.blockNumber;
        req.BlockHash   = m_paymentDetails.blockHash;

        std::string raw_resp;
        int http_status = 0;
        ErrorResponse err_resp;
        chrono::steady_clock::time_point start = chrono::steady_clock::now();
        chrono::milliseconds elapsed;

        do {

            bool r = invoke_http_rest("/dapi/v2.0/get_payment_data", req, raw_resp, err_resp, m_http_client, http_status, m_network_timeout, "POST");
            if (!r) {
                MERROR("Failed to invoke sale: " << graft::to_json_str(err_resp));
                return r;
            }

            if (http_status == 200 && !raw_resp.empty())
                break;

            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(chrono::steady_clock::now() - start);

        } while (elapsed < m_sale_timeout);

        if (raw_resp.empty()) {
            MERROR("Failed to get payment data for payment: " << req.PaymentID);
            return false;
        }


        if (!graft::from_json_str(raw_resp, m_paymentDataResponse)) {
            MERROR("Failed to parse payment data response: " << raw_resp);
            return false;
        }

        MINFO("Payment data received for payment id: " << req.PaymentID << " " << raw_resp);

        std::string encryptedPaymentInfoBlob;
        if (!epee::string_tools::parse_hexstr_to_binbuff(m_paymentDataResponse.paymentData.EncryptedPayment, encryptedPaymentInfoBlob)) {
            MERROR("Failed to deserialize EncryptedPayment from: " << m_paymentDataResponse.paymentData.EncryptedPayment);
            return false;
        }

        std::string paymentInfoStr;
        if (!graft::crypto_tools::decryptMessage(encryptedPaymentInfoBlob, m_decryption_key, paymentInfoStr)) {
            MERROR("Failed to decrypt EncryptedPaymentInfo witrh");
            return false;
        }

        if (!graft::from_json_str(paymentInfoStr, m_paymentInfo)) {
            MERROR("Failed to deserialize payment info from: " << paymentInfoStr);
        }

        // decrypt payment details
        if (!epee::string_tools::parse_hexstr_to_binbuff(m_paymentInfo.Details, encryptedPaymentInfoBlob)) {
            MERROR("Failed to deserialize PaymentInfo from: " << m_paymentInfo.Details);
            return false;
        }

        if (!graft::crypto_tools::decryptMessage(encryptedPaymentInfoBlob, m_decryption_key, m_paymentInfo.Details)) {
            MERROR("Failed to decrypt payment info from "    << paymentInfoStr);
            return false;
        }

        MINFO("payment info: " << m_paymentInfo.Details);

        return true;
    }

    bool pay()
    {
      // 1. collect auth sample wallet addresses, WalletProxy and PosProxy wallet addresses
        SupernodeInfoRequest req;
        for (const auto &item : m_paymentDataResponse.paymentData.AuthSampleKeys) {
            req.input.push_back(item.Id);
        }
        SupernodeInfoResponse resp;
        std::string raw_resp;
        ErrorResponse err_resp;
        int http_status;
        bool r = invoke_http_rest("/dapi/v2.0/core/get_supernode_info", req, raw_resp, err_resp, m_http_client, http_status, m_network_timeout, "POST");

        if (!r || http_status != 200) {
            MERROR("Failed to invoke get_payment_data: " << raw_resp);
            return false;
        }

        if (!graft::from_json_str(raw_resp, resp)) {
            MERROR("Failed to parse supernode info response: " << raw_resp);
            return false;
        }

        if (m_paymentDataResponse.paymentData.AuthSampleKeys.size() != resp.output.size()) {
            MERROR("Wrong amount of supernodes: requested: " << m_paymentDataResponse.paymentData.AuthSampleKeys.size()
                   << ", got: " << resp.output.size());
            return false;
        }

        std::vector<std::string> fee_wallets;
        fee_wallets.push_back(m_paymentDataResponse.paymentData.PosProxy.WalletAddress);
        fee_wallets.push_back(m_paymentDataResponse.WalletProxy.WalletAddress);
        for (const auto &auth_sample_member : resp.output) {
            fee_wallets.push_back(auth_sample_member.Address);
        }
      // 2. create tx;
        cryptonote::rta_header rta_hdr;
        rta_hdr.payment_id = m_paymentDetails.paymentId;
        rta_hdr.auth_sample_height = m_paymentDetails.blockNumber;

        if (!append_key_to_rta_hdr(rta_hdr, m_paymentDetails.posAddress.Id)) { // pos key;
            return false;
        }
        if (!append_key_to_rta_hdr(rta_hdr, m_paymentDataResponse.paymentData.PosProxy.Id)) { // pos proxy
            return false;
        }
        if (!append_key_to_rta_hdr(rta_hdr, m_paymentDataResponse.WalletProxy.Id)) { // wallet proxy
            return false;
        }

        for (const auto & item: m_paymentDataResponse.paymentData.AuthSampleKeys) {
            if (!append_key_to_rta_hdr(rta_hdr, item.Id)) {
                return false;
            }
        }

        std::vector<uint8_t> extra;
        cryptonote::add_graft_rta_header_to_extra(extra, rta_hdr);
        uint64_t recepient_amount, fee_per_destination = 0;
        double fee_ratio = 0.05;

        std::vector<tools::wallet2::pending_tx> ptx_v = m_wallet.create_transactions_graft(
                    m_paymentDetails.posAddress.WalletAddress,
                    fee_wallets,
                    m_paymentInfo.Amount,
                    fee_ratio,
                    0,
                    0,
                    extra,
                    0,
                    {0},
                    recepient_amount,
                    fee_per_destination);
        if (ptx_v.size() < 1 || ptx_v.size() > 1) {
            MERROR("wrong tx count: " << ptx_v.size());
            return false;
        }

        const cryptonote::transaction &tx = ptx_v.at(0).tx;

        MINFO("About to do pay, payment_id:  " << m_paymentDetails.paymentId << ", Total amount: " << print_money(m_paymentInfo.Amount)
              << ", Merchant amount : " << print_money(recepient_amount)
              << ", Fee per member: " << print_money(fee_per_destination)
              << ", Payment details: " << m_paymentInfo.Details
              << ", tx_id: " << tx.hash);

        // 3. get tx private key
        std::string buf;
        PayRequest pay_req;

        std::string tx_buf(reinterpret_cast<const char*>(&ptx_v.at(0).tx_key), sizeof(crypto::secret_key));
        graft::crypto_tools::encryptMessage(tx_buf, rta_hdr.keys, buf);
        pay_req.TxKey = epee::string_tools::buff_to_hex_nodelimer(buf);
        buf.clear();
        graft::crypto_tools::encryptMessage(cryptonote::tx_to_blob(tx), rta_hdr.keys, buf);
        pay_req.TxBlob = epee::string_tools::buff_to_hex_nodelimer(buf);

        r = invoke_http_rest("/dapi/v2.0/pay", pay_req, raw_resp, err_resp, m_http_client, http_status, m_network_timeout, "POST");

        if (!r || http_status != 202) {
            MERROR("Failed to invoke pay: " << raw_resp);
            return false;
        }

        MINFO("Pay processed, payment id: " << m_paymentDetails.paymentId << ", tx id: " << tx.hash);

        return true;
    }

    std::string paymentId() const
    {
        return m_payment_id;
    }


private:
    std::string m_qrcode_file;
    std::chrono::milliseconds m_sale_timeout = std::chrono::milliseconds(5000);
    epee::net_utils::http::http_simple_client m_http_client;
    std::chrono::seconds m_network_timeout = std::chrono::seconds(10);
    std::string m_wallet_file;
    std::string m_payment_id;
    WalletDataQrCode m_paymentDetails;
    tools::wallet2 m_wallet;
    crypto::secret_key m_decryption_key;
    PaymentInfo m_paymentInfo;
    PaymentDataResponse m_paymentDataResponse;
};




int main(int argc, char* argv[])
{
    TRY_ENTRY();
    epee::string_tools::set_module_name_and_folder(argv[0]);

    uint32_t log_level = 0;
    std::string m_config_folder;

    tools::on_startup();

    po::options_description desc_cmd("Command line options");

    const command_line::arg_descriptor<std::string> arg_input_file = {"qrcode-file", "Specify file where to write qr-code", "wallet-qr-code.json", false};
    const command_line::arg_descriptor<std::string> arg_log_level   = {"log-level",  "0-4 or categories", "", false};
    const command_line::arg_descriptor<size_t>      arg_sale_timeout  = {"sale-timeout", "Sale timeout in millis", 5000, false};
    const command_line::arg_descriptor<std::string> arg_supernode_address = { "supernode-address", "Supernode address", "localhost:28690", false };
    const command_line::arg_descriptor<std::string> arg_cryptonode_address = { "cryptonode-address", "Cryptonode address", "localhost:28681", false };
    const command_line::arg_descriptor<std::string> arg_wallet_path = { "wallet-path", "Wallet path", "wallet", false };
    const command_line::arg_descriptor<std::string> arg_wallet_password = { "wallet-password", "Wallet password", "", false };


    command_line::add_arg(desc_cmd, arg_input_file);
    command_line::add_arg(desc_cmd, arg_log_level);

    command_line::add_arg(desc_cmd, arg_sale_timeout);
    command_line::add_arg(desc_cmd, arg_supernode_address);
    command_line::add_arg(desc_cmd, arg_cryptonode_address);
    command_line::add_arg(desc_cmd, arg_wallet_path);
    command_line::add_arg(desc_cmd, arg_wallet_password);
    command_line::add_arg(desc_cmd, command_line::arg_help);

    po::options_description desc_options("Allowed options");
    desc_options.add(desc_cmd);

    po::variables_map vm;

    bool r = command_line::handle_error_helper(desc_options, [&]()
    {
        po::store(po::parse_command_line(argc, argv, desc_options), vm);
        po::notify(vm);
        return true;
    });
    if (!r)
        return EXIT_FAILURE;


    if (command_line::get_arg(vm, command_line::arg_help))
    {
        std::cout << desc_options << std::endl;
        return EXIT_FAILURE;
    }

    mlog_configure(mlog_get_default_log_path("rta-pos-cli.log"), true);
    if (!command_line::is_arg_defaulted(vm, arg_log_level))
        mlog_set_log(command_line::get_arg(vm, arg_log_level).c_str());
    else
        mlog_set_log(std::string(std::to_string(log_level) + ",rta-pos-cli:INFO").c_str());

    MINFO("Starting...");

    boost::filesystem::path fs_import_file_path;

    std::cout << "qrcode-file: " << command_line::get_arg(vm, arg_input_file) << std::endl;
    std::cout << "log-level: "   << command_line::get_arg(vm, arg_log_level) << std::endl;
    std::cout << "sale-itimeout: "      << command_line::get_arg(vm, arg_sale_timeout) << std::endl;
    std::cout << "supernode-address: "  << command_line::get_arg(vm, arg_supernode_address) << std::endl;
    std::cout << "wallet-path: " << command_line::get_arg(vm, arg_wallet_path) << std::endl;

    Wallet wallet(command_line::get_arg(vm, arg_input_file),
            command_line::get_arg(vm, arg_supernode_address),
            command_line::get_arg(vm, arg_cryptonode_address),
            command_line::get_arg(vm, arg_sale_timeout),
            command_line::get_arg(vm, arg_wallet_path),
            command_line::get_arg(vm, arg_wallet_password)
            );

    if (!wallet.readPaymentDetails()) {
        MERROR("Failed to read payment from QR code");
        return EXIT_FAILURE;
    }

    if (!wallet.getPaymentData()) {
        MERROR("Failed to get payment data");
        return EXIT_FAILURE;
    }

    if (!wallet.pay()) {
        MERROR("pay failed");
        return EXIT_FAILURE;
    }

    // TODO: monitor for a payment status

    return 0;

    CATCH_ENTRY("App error", 1);
}
