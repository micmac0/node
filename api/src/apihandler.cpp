#include <apihandler.hpp>

#include <csnode/conveyer.hpp>
#include <lib/system/logger.hpp>
#include <lib/system/utils.hpp>
#include <solver/smartcontracts.hpp>
#include <src/priv_crypto.hpp>
#include "csconnector/csconnector.hpp"
#include "stdafx.h"
#include <csnode/fee.hpp>

#include <base58.h>

constexpr csdb::user_field_id_t kSmartStateIndex = ~1;
using namespace api;
using namespace ::apache;

apiexec::APIEXECHandler::APIEXECHandler(BlockChain& blockchain, cs::SolverCore& solver, executor::Executor& executor, const csconnector::Config& config)
: executor_(executor)
, blockchain_(blockchain)
, solver_(solver) {
    csunused(config);
}

APIHandler::APIHandler(BlockChain& blockchain, cs::SolverCore& _solver, executor::Executor& executor, const csconnector::Config&)
: executor_(executor)
, s_blockchain(blockchain)
, solver(_solver)
#ifdef MONITOR_NODE
, stats(blockchain)
#endif
, tm(this) {
#ifdef MONITOR_NODE
    if (static bool firstTime = true; firstTime) {
        stats_.second.resize(::csstats::collectionPeriods.size());
        auto nowGlobal = std::chrono::system_clock::now();
        auto lastTimePoint = nowGlobal - std::chrono::seconds(::csstats::collectionPeriods[::csstats::PeriodIndex::Month]);

        for (auto time = nowGlobal; time > lastTimePoint; time -= std::chrono::seconds(::csstats::updateTimeSec)) {
            ::csstats::PeriodStats cut;
            cut.timeStamp = time;
            stats_.first.push_back(cut);
        }
        firstTime = false;
    }
#endif
}

void APIHandler::run() {
    if (!s_blockchain.isGood()) {
        return;
    }

#ifdef MONITOR_NODE
    stats.run(stats_);
#endif
    tm.run();  // Run this AFTER updating all the caches for maximal efficiency

    state_updater_running.test_and_set(std::memory_order_acquire);
    state_updater = std::thread([this]() { state_updater_work_function(); });
}

APIHandler::~APIHandler() {
    state_updater_running.clear(std::memory_order_release);

    if (state_updater.joinable()) {
        state_updater.join();
    }
}

template <typename ResultType>
bool validatePagination(ResultType& _return, APIHandler& handler, int64_t offset, int64_t limit) {
    if (offset < 0 || limit <= 0 || limit > 100) {
        handler.SetResponseStatus(_return.status, APIHandlerBase::APIRequestStatusType::FAILURE);
        return false;
    }

    return true;
}

void APIHandler::state_updater_work_function() {
    try {
        auto lasthash = s_blockchain.getLastHash();
        while (state_updater_running.test_and_set(std::memory_order_acquire)) {
            if (!update_smart_caches_once(lasthash)) {
                {
                    std::unique_lock lk(dbLock_);
                    newBlockCv_.wait(lk);
                }
                lasthash = s_blockchain.getLastHash();
            }
        }
    }
    catch (std::exception& ex) {
        std::stringstream ss;
        ss << "error [" << ex.what() << "] in file'" << __FILE__ << "' line'" << __LINE__ << "'";
        cserror() << ss.str().c_str();
    }
    catch (...) {
        std::stringstream ss;
        ss << "unknown error in file'" << __FILE__ << "' line'" << __LINE__ << "'";
        cslog() << ss.str().c_str();
    }
}

void APIHandlerBase::SetResponseStatus(general::APIResponse& response, APIRequestStatusType status, const std::string& details) {
    struct APIRequestStatus {
        APIRequestStatus(uint8_t code, std::string message)
        : message(message)
        , code(code) {
        }
        std::string message;
        uint8_t code;
    };

    APIRequestStatus statuses[static_cast<size_t>(APIHandlerBase::APIRequestStatusType::MAX)] = {
        {0, "Success"},
        {1, "Failure"},
        {2, "Not Implemented"},
        {3, "Not found"},
		{4, "Transaction in progress"}
    };
    response.code = statuses[static_cast<uint8_t>(status)].code;
    response.message = statuses[static_cast<uint8_t>(status)].message + details;
}

void APIHandlerBase::SetResponseStatus(general::APIResponse& response, bool commandWasHandled) {
    SetResponseStatus(response, (commandWasHandled ? APIRequestStatusType::SUCCESS : APIRequestStatusType::NOT_IMPLEMENTED));
}

void APIHandler::WalletDataGet(WalletDataGetResult& _return, const general::Address& address) {
    const csdb::Address addr = BlockChain::getAddressFromKey(address);
    BlockChain::WalletData wallData{};
    BlockChain::WalletId wallId{};
    if (!s_blockchain.findWalletData(addr, wallData, wallId)) {
        SetResponseStatus(_return.status, APIRequestStatusType::NOT_FOUND);
        return;
    }
    _return.walletData.walletId = wallId;
    _return.walletData.balance.integral = wallData.balance_.integral();
    _return.walletData.balance.fraction = static_cast<decltype(_return.walletData.balance.fraction)>(wallData.balance_.fraction());
    const cs::TransactionsTail& tail = wallData.trxTail_;
    _return.walletData.lastTransactionId = tail.empty() ? 0 : tail.getLastTransactionId();

    SetResponseStatus(_return.status, APIRequestStatusType::SUCCESS);
}

void APIHandler::WalletIdGet(api::WalletIdGetResult& _return, const general::Address& address) {
    const csdb::Address addr = BlockChain::getAddressFromKey(address);
    BlockChain::WalletData wallData{};
    BlockChain::WalletId wallId{};
    if (!s_blockchain.findWalletData(addr, wallData, wallId)) {
        SetResponseStatus(_return.status, APIRequestStatusType::NOT_FOUND);
        return;
    }

    _return.walletId = wallId;
    SetResponseStatus(_return.status, APIRequestStatusType::SUCCESS);
}

void APIHandler::WalletTransactionsCountGet(api::WalletTransactionsCountGetResult& _return, const general::Address& address) {
    const csdb::Address addr = BlockChain::getAddressFromKey(address);
    BlockChain::WalletData wallData{};
    BlockChain::WalletId wallId{};
    if (!s_blockchain.findWalletData(addr, wallData, wallId)) {
        SetResponseStatus(_return.status, APIRequestStatusType::NOT_FOUND);
        return;
    }
    _return.lastTransactionInnerId = wallData.trxTail_.empty() ? 0 : wallData.trxTail_.getLastTransactionId();
    SetResponseStatus(_return.status, APIRequestStatusType::SUCCESS);
}

void APIHandler::WalletBalanceGet(api::WalletBalanceGetResult& _return, const general::Address& address) {
    const csdb::Address addr = BlockChain::getAddressFromKey(address);
    BlockChain::WalletData wallData{};
    BlockChain::WalletId wallId{};
    if (!s_blockchain.findWalletData(addr, wallData, wallId)) {
        SetResponseStatus(_return.status, APIRequestStatusType::NOT_FOUND);
        return;
    }
    _return.balance.integral = wallData.balance_.integral();
    _return.balance.fraction = static_cast<decltype(_return.balance.fraction)>(wallData.balance_.fraction());
    SetResponseStatus(_return.status, APIRequestStatusType::SUCCESS);
}

std::string fromByteArray(const cs::Bytes& bar) {
    std::string res;
    {
        res.reserve(bar.size());
        std::transform(bar.begin(), bar.end(), std::back_inserter<std::string>(res), [](uint8_t _) { return char(_); });
    }
    return res;
}

std::string fromByteArray(const cs::PublicKey& bar) {
    std::string res;
    {
        res.reserve(bar.size());
        std::transform(bar.begin(), bar.end(), std::back_inserter<std::string>(res), [](uint8_t _) { return char(_); });
    }
    return res;
}

cs::Bytes toByteArray(const std::string& s) {
    cs::Bytes res;
    {
        res.reserve(s.size());
        std::transform(s.begin(), s.end(), std::back_inserter<decltype(res)>(res), [](uint8_t _) { return uint8_t(_); });
    }
    return res;
}

general::Amount convertAmount(const csdb::Amount& amount) {
	general::Amount result;
    result.integral = amount.integral();
    result.fraction = amount.fraction();
    assert(result.fraction >= 0);
    return result;
}

api::TransactionId convert_transaction_id(const csdb::TransactionID& trid) {
    api::TransactionId result_id;
    result_id.index = (uint32_t) trid.index();
    result_id.poolHash = fromByteArray(trid.pool_hash().to_binary());
    return result_id;
}

csdb::TransactionID convert_transaction_id(const api::TransactionId& trid) {
    return csdb::TransactionID(csdb::PoolHash::from_binary(toByteArray(trid.poolHash)), trid.index);
}

bool is_smart(const csdb::Transaction& tr) {
    using namespace cs::trx_uf;
    // deploy::Code == start::Methods == 0
    return tr.user_field(deploy::Code).type() == csdb::UserField::Type::String;
}

bool is_smart_state(const csdb::Transaction& tr) {
    using namespace cs::trx_uf;
    // test user_field[RefStart] helps filter out ancient smart contracts:
    return (tr.user_field(new_state::Value).type() == csdb::UserField::Type::String && tr.user_field(new_state::RefStart).type() == csdb::UserField::Type::String);
}

bool is_smart_deploy(const api::SmartContractInvocation& smart) {
    return smart.method.empty();
}

bool is_deploy_transaction(const csdb::Transaction& tr) {
    using namespace cs::trx_uf;
    auto uf = tr.user_field(deploy::Code);
    return uf.type() == csdb::UserField::Type::String && is_smart_deploy(deserialize<api::SmartContractInvocation>(uf.value<std::string>()));
}

APIHandler::SmartOperation APIHandler::getSmartStatus(const csdb::TransactionID tId) {
    auto sop = lockedReference(smart_operations);
    auto it = sop->find(tId);
    if (it == sop->end())
        return SmartOperation();
    return it->second;
}

template <typename SmartOp, typename TransInfo>
static void fillTransInfoWithOpData(const SmartOp& op, TransInfo& ti) {
    ti.state = (api::SmartOperationState)(uint32_t)op.state;
    if (op.stateTransaction.is_valid())
        ti.__set_stateTransaction(convert_transaction_id(op.stateTransaction));
}

api::SealedTransaction APIHandler::convertTransaction(const csdb::Transaction& transaction) {
    api::SealedTransaction result;
    const csdb::Amount amount = transaction.amount();
    csdb::Currency currency = transaction.currency();
    csdb::Address address = transaction.source();
    if (address.is_wallet_id()) {
        BlockChain::WalletData data_to_fetch_pulic_key;
        s_blockchain.findWalletData(transaction.source().wallet_id(), data_to_fetch_pulic_key);
        address = csdb::Address::from_public_key(data_to_fetch_pulic_key.address_);
    }

    csdb::Address target = transaction.target();
    if (target.is_wallet_id()) {
        BlockChain::WalletData data_to_fetch_pulic_key;
        s_blockchain.findWalletData(transaction.target().wallet_id(), data_to_fetch_pulic_key);
        target = csdb::Address::from_public_key(data_to_fetch_pulic_key.address_);
    }

    result.id = convert_transaction_id(transaction.id());
    result.__isset.id = true;
    result.__isset.trxn = true;
    result.trxn.id = transaction.innerID();
    result.trxn.amount = convertAmount(amount);
    result.trxn.currency = DEFAULT_CURRENCY;
    result.trxn.source = fromByteArray(address.public_key());
    result.trxn.target = fromByteArray(target.public_key());
    result.trxn.fee.commission = transaction.counted_fee().get_raw();

    result.trxn.timeCreation = transaction.get_time();

    result.trxn.poolNumber = s_blockchain.loadBlock(transaction.id().pool_hash()).sequence();

    if (is_smart(transaction)) {
        using namespace cs::trx_uf;
        auto sci = deserialize<api::SmartContractInvocation>(transaction.user_field(deploy::Code).value<std::string>());
        bool isToken = false;

        auto smartResult = getSmartStatus(transaction.id());
        result.trxn.__set_smartInfo(api::SmartTransInfo{});

        if (is_smart_deploy(sci)) {
            result.trxn.type = api::TransactionType::TT_SmartDeploy;
            tm.applyToInternal([&isToken, &target, &result](const TokensMap& tokens, const HoldersMap&) {
                auto it = tokens.find(target);
                if (it != tokens.end()) {
                    isToken = true;
                    api::TokenDeployTransInfo dti;
                    dti.name = it->second.name;
                    dti.code = it->second.symbol;
                    dti.standart = (api::TokenStandart)(uint32_t)it->second.standart;
                    result.trxn.smartInfo.__set_v_tokenDeploy(dti);
                }
            });

            if (isToken)
                fillTransInfoWithOpData(smartResult, result.trxn.smartInfo.v_tokenDeploy);
            else {
                result.trxn.smartInfo.__set_v_smartDeploy(SmartDeployTransInfo());
                fillTransInfoWithOpData(smartResult, result.trxn.smartInfo.v_smartDeploy);
            }
        }
        else {
            bool isTransfer = TokensMaster::isTransfer(sci.method, sci.params);
            result.trxn.type = api::TransactionType::TT_SmartExecute;
            if (isTransfer) {
                tm.applyToInternal([&isToken, &isTransfer, &target, &result](const TokensMap& tokens, const HoldersMap&) {
                    auto it = tokens.find(target);
                    if (it != tokens.end()) {
                        isToken = true;
                        api::TokenTransferTransInfo tti;
                        tti.code = it->second.symbol;
                        result.trxn.smartInfo.__set_v_tokenTransfer(tti);
                    }
                    else
                        isTransfer = false;
                });
            }

            if (isTransfer) {
                auto addrPair = TokensMaster::getTransferData(address, sci.method, sci.params);

                result.trxn.smartInfo.v_tokenTransfer.sender = fromByteArray(addrPair.first.public_key());
                result.trxn.smartInfo.v_tokenTransfer.receiver = fromByteArray(addrPair.second.public_key());
                result.trxn.smartInfo.v_tokenTransfer.amount = TokensMaster::getAmount(sci);

                if (smartResult.hasReturnValue())
                    result.trxn.smartInfo.v_tokenTransfer.__set_transferSuccess(smartResult.getReturnedBool());

                fillTransInfoWithOpData(smartResult, result.trxn.smartInfo.v_tokenTransfer);
            }
            else {
                SmartExecutionTransInfo eti;
                eti.method = sci.method;
                eti.params = sci.params;
                fillTransInfoWithOpData(smartResult, eti);

                result.trxn.smartInfo.__set_v_smartExecution(eti);
            }
        }

        result.trxn.__set_smartContract(sci);
    }
    else if (is_smart_state(transaction)) {
        result.trxn.type = api::TransactionType::TT_SmartState;
        api::SmartStateTransInfo sti;
        sti.success = !(transaction.user_field(cs::trx_uf::new_state::Value).value<std::string>().empty());
        sti.executionFee = convertAmount(transaction.user_field(cs::trx_uf::new_state::Fee).value<csdb::Amount>());
        cs::SmartContractRef scr;
        scr.from_user_field(transaction.user_field(cs::trx_uf::new_state::RefStart));
        sti.startTransaction = convert_transaction_id(scr.getTransactionID());

        auto fld = transaction.user_field(cs::trx_uf::new_state::RetVal);
        if (fld.is_valid()) {
            auto retVal = fld.value<std::string>();
            auto variant = deserialize<::general::Variant>(std::move(retVal));
            // override retValue with text message if new state is empty
            if (sti.success) {
                sti.__set_returnValue(variant);
            }
            else {
                if (variant.__isset.v_byte) {
                    // if not success and variant is of byte type there is an error code
                    variant.__set_v_string(cs::SmartContracts::get_error_message(variant.v_byte));
                }
                sti.__set_returnValue(variant);
            }
        }
        result.trxn.smartInfo.__set_v_smartState(sti);
        result.trxn.__isset.smartInfo = true;
    }
    else {
        result.trxn.type = api::TransactionType::TT_Normal;
        auto ufd = transaction.user_field(1);
        if (ufd.is_valid())
            result.trxn.__set_userFields(ufd.value<std::string>());
    }

    // fill ExtraFee
    // 1) find state transaction
    csdb::Transaction stateTrx;
    if (is_smart(transaction)) {
        auto opers = lockedReference(this->smart_operations);
        stateTrx = s_blockchain.loadTransaction((*opers)[transaction.id()].stateTransaction);
    }
    else if (is_smart_state(transaction))
        stateTrx = transaction;

    if (!is_smart_state(stateTrx))
        return result;

    // 2) fill ExtraFee for state transaction
    auto pool = s_blockchain.loadBlock(stateTrx.id().pool_hash());
    auto transactions = pool.transactions();
    ExtraFee extraFee;
    extraFee.transactionId = convert_transaction_id(stateTrx.id());
    // 2.1) counted_fee 
    extraFee.sum = convertAmount(csdb::Amount(stateTrx.counted_fee().to_double()));
    extraFee.comment = "contract state fee";
    result.trxn.extraFee.push_back(extraFee);
    // 2.2) execution fee
    extraFee.sum = convertAmount(stateTrx.user_field(cs::trx_uf::new_state::Fee).value<csdb::Amount>());
    extraFee.comment = "contract execution fee";
    result.trxn.extraFee.push_back(extraFee);

    // 3) fill ExtraFee for extra transactions
    auto trxIt = std::find_if(transactions.begin(), transactions.end(), [&stateTrx](const csdb::Transaction& ptrx) { return ptrx.id() == stateTrx.id(); });
    for (auto trx = ++trxIt; trx != transactions.end(); ++trx) {
        if (s_blockchain.getAddressByType(trx->source(), BlockChain::AddressType::PublicKey) != 
            s_blockchain.getAddressByType(stateTrx.source(), BlockChain::AddressType::PublicKey)) // end find extra transactions                   
                break;
        extraFee.transactionId = convert_transaction_id(trx->id());
        extraFee.sum = convertAmount(csdb::Amount(trx->counted_fee().to_double()));
        extraFee.comment = "extra fee";
        result.trxn.extraFee.push_back(extraFee);
    }

    return result;
}

std::vector<api::SealedTransaction> APIHandler::convertTransactions(const std::vector<csdb::Transaction>& transactions) {
    std::vector<api::SealedTransaction> result;
    result.resize(transactions.size());
    /*const auto convert = std::bind(&APIHandler::convertTransaction, this, std::placeholders::_1);
    std::transform(transactions.begin(), transactions.end(), result.begin(), convert);
    for (auto& it : result) {
      auto poolHash = csdb::PoolHash::from_binary(toByteArray(it.id.poolHash));
      it.trxn.timeCreation = convertPool(poolHash).time;

      auto smartResult = getSmartStatus(it.trxn.id());
      it.trxn.__set_smartInfo(api::SmartTransInfo{});
      it.trxn.smartInfo.__set_v_smartDeploy(SmartDeployTransInfo());
      fillTransInfoWithOpData(smartResult, trxn.smartInfo.v_smartDeploy);
    }*/
    for (size_t Count = 0; Count < result.size(); Count++) {
        result[Count] = convertTransaction(transactions[Count]);
    }
    return result;
}

api::Pool APIHandler::convertPool(const csdb::Pool& pool) {
    api::Pool result;
    pool.is_valid();
    if (pool.is_valid()) {
        result.hash = fromByteArray(pool.hash().to_binary());
        result.poolNumber = pool.sequence();
        assert(result.poolNumber >= 0);
        result.prevHash = fromByteArray(pool.previous_hash().to_binary());
        result.time = pool.get_time();

        result.transactionsCount = (int32_t)pool.transactions_count();  // DO NOT EVER CREATE POOLS WITH
                                                                        // MORE THAN 2 BILLION
                                                                        // TRANSACTIONS, EVEN AT NIGHT

        const auto& wpk = pool.writer_public_key();
        result.writer = fromByteArray(cs::Bytes(wpk.begin(), wpk.end()));

        double totalFee = 0;
        const auto& transs = const_cast<csdb::Pool&>(pool).transactions();
        for (auto& t : transs)
            totalFee += t.counted_fee().to_double();

        const auto tf = csdb::Amount(totalFee);
        result.totalFee.integral = tf.integral();
        result.totalFee.fraction = tf.fraction();
    }
    return result;
}

api::Pool APIHandler::convertPool(const csdb::PoolHash& poolHash) {
    return convertPool(s_blockchain.loadBlock(poolHash));
}

std::vector<api::SealedTransaction> APIHandler::extractTransactions(const csdb::Pool& pool, int64_t limit, const int64_t offset) {
    int64_t transactionsCount = pool.transactions_count();
    assert(transactionsCount >= 0);
    std::vector<api::SealedTransaction> result;
    if (offset > transactionsCount)
        return result;  // если запрашиваемые // транзакций выходят за // пределы пула возвращаем пустой результат
    transactionsCount -= offset;  // мы можем отдать все транзакции в пуле за вычетом смещения
    if (limit > transactionsCount)
        limit = transactionsCount;  // лимит уменьшается до реального количества // транзакций которые можно отдать
    for (int64_t index = offset; index < (offset + limit); ++index) {
        result.push_back(convertTransaction(pool.transaction(index)));
    }
    return result;
}

void APIHandler::TransactionGet(TransactionGetResult& _return, const TransactionId& transactionId) {
    const csdb::PoolHash poolhash = csdb::PoolHash::from_binary(toByteArray(transactionId.poolHash));
    const csdb::TransactionID tmpTransactionId = csdb::TransactionID(poolhash, (transactionId.index));
    csdb::Transaction transaction = s_blockchain.loadTransaction(tmpTransactionId);
    _return.found = transaction.is_valid();
    if (_return.found)
        _return.transaction = convertTransaction(transaction);

    SetResponseStatus(_return.status, APIRequestStatusType::SUCCESS, std::to_string(transaction.counted_fee().to_double()));
}

void APIHandler::TransactionsGet(TransactionsGetResult& _return, const general::Address& address, const int64_t _offset, const int64_t limit) {
    const csdb::Address addr = BlockChain::getAddressFromKey(address);
    BlockChain::Transactions transactions;
    if (limit > 0) {
        const int64_t offset = (_offset < 0) ? 0 : _offset;
        s_blockchain.getTransactions(transactions, addr, static_cast<uint64_t>(offset), static_cast<uint64_t>(limit));
    }
    _return.transactions = convertTransactions(transactions);

#ifdef MONITOR_NODE
    _return.total_trxns_count = s_blockchain.getTransactionsCount(addr);
#endif

    SetResponseStatus(_return.status, APIRequestStatusType::SUCCESS);
}

api::SmartContractInvocation fetch_smart(const csdb::Transaction& tr) {
    if (tr.is_valid()) {
        const auto uf = tr.user_field(cs::trx_uf::deploy::Code);
        if (uf.is_valid()) {
            std::string data = uf.value<std::string>();
            if (!data.empty()) {
                return deserialize<api::SmartContractInvocation>(std::move(data));
            }
        }
    }
    return api::SmartContractInvocation();
}

api::SmartContract APIHandler::fetch_smart_body(const csdb::Transaction& tr) {
    using namespace cs::trx_uf;
    api::SmartContract res;
    if (!tr.is_valid())
        return res;
    const auto sci = deserialize<api::SmartContractInvocation>(tr.user_field(deploy::Code).value<std::string>());
    res.smartContractDeploy.byteCodeObjects = sci.smartContractDeploy.byteCodeObjects;
    res.smartContractDeploy.sourceCode = sci.smartContractDeploy.sourceCode;
    res.smartContractDeploy.hashState = sci.smartContractDeploy.hashState;
    res.deployer = fromByteArray(s_blockchain.getAddressByType(tr.source(), BlockChain::AddressType::PublicKey).public_key());
    res.address = fromByteArray(s_blockchain.getAddressByType(tr.target(), BlockChain::AddressType::PublicKey).public_key());

#ifdef TOKENS_CACHE
    tm.applyToInternal([&tr, &res](const TokensMap& tokens, const HoldersMap&) {
        auto it = tokens.find(tr.target());
        if (it != tokens.end())
            res.smartContractDeploy.tokenStandart = (api::TokenStandart)(uint32_t)it->second.standart;
        else
            res.smartContractDeploy.tokenStandart = TokenStandart::NotAToken;
    });
#else
    res.smartContractDeploy.tokenStandart = TokenStandart::NotAToken;
#endif

#ifdef MONITOR_NODE
    s_blockchain.applyToWallet(tr.target(), [&res](const cs::WalletsCache::WalletData& wd) { res.createTime = wd.createTime_; });
#endif
	if (tr.user_field(0).is_valid())
		res.transactionsCount = s_blockchain.getTransactionsCount(tr.target());

    auto pool = s_blockchain.loadBlock(tr.id().pool_hash());
    res.createTime = pool.get_time();

    return res;
}

template <typename T>
auto set_max_fee(T& trx, const csdb::Amount& am, int) -> decltype(trx.set_max_fee(am), void()) {
    trx.set_max_fee(am);
}

template <typename T>
void set_max_fee(T&, const csdb::Amount&, long) {
}

csdb::Transaction APIHandler::make_transaction(const Transaction& transaction) {
    csdb::Transaction send_transaction;
    const auto source = BlockChain::getAddressFromKey(transaction.source);
    const uint64_t WALLET_DENOM = csdb::Amount::AMOUNT_MAX_FRACTION;  // 1'000'000'000'000'000'000ull;
    send_transaction.set_amount(csdb::Amount(transaction.amount.integral, transaction.amount.fraction, WALLET_DENOM));
    BlockChain::WalletData wallData{};
    BlockChain::WalletId id{};

    if (!transaction.smartContract.forgetNewState &&  // not for getter
            !s_blockchain.findWalletData(source, wallData, id)) {
        return csdb::Transaction{};
    }

    send_transaction.set_currency(csdb::Currency(1));
    send_transaction.set_source(source);
    send_transaction.set_target(BlockChain::getAddressFromKey(transaction.target));
    send_transaction.set_max_fee(csdb::AmountCommission((uint16_t)transaction.fee.commission));
    send_transaction.set_innerID(transaction.id & 0x3fffffffffff);

    // TODO Change Thrift to avoid copy
    cs::Signature signature{};

    if (transaction.signature.size() == signature.size()) {
        std::copy(transaction.signature.begin(), transaction.signature.end(), signature.begin());
    }

    send_transaction.set_signature(signature);
    return send_transaction;
}

std::string get_delimited_transaction_sighex(const csdb::Transaction& tr) {
    auto bs = fromByteArray(tr.to_byte_stream_for_sig());
    return std::string({' '}) + cs::Utils::byteStreamToHex(bs.data(), bs.length());
}

void APIHandler::dumb_transaction_flow(api::TransactionFlowResult& _return, const Transaction& transaction) {
    auto tr = make_transaction(transaction);
    if (!transaction.userFields.empty()) {
        tr.add_user_field(1, transaction.userFields);
    }

    // check money
    const auto source_addr = s_blockchain.getAddressByType(tr.source(), BlockChain::AddressType::PublicKey);
    BlockChain::WalletData wallData{};
    BlockChain::WalletId wallId{};
    if (!s_blockchain.findWalletData(source_addr, wallData, wallId)) {
        _return.status.code = ERROR_CODE;
        _return.status.message = "wallet not found!";
        return;
    }

    const auto max_sum = tr.amount().to_double() + kMinFee;
    const auto balance = wallData.balance_.to_double();
    if (max_sum > balance) {
        cslog() << "API: reject transaction with insufficient balance, max_sum = " << max_sum << ", balance = " << balance;
        _return.status.code = ERROR_CODE;
        _return.status.message = "not enough money!\nmax_sum: " + std::to_string(max_sum) + "\nbalance: " + std::to_string(balance);
        return;
    }

    // check max fee
    if (csdb::AmountCommission countedFee; !cs::fee::estimateMaxFee(tr, countedFee)) {
        _return.status.code = ERROR_CODE;
        _return.status.message = "max fee is not enough, counted fee will be " + std::to_string(countedFee.to_double());
        return;
    }

    // check signature
    const auto byteStream = tr.to_byte_stream_for_sig();
    if (!cscrypto::verifySignature(tr.signature(), s_blockchain.getAddressByType(tr.source(), BlockChain::AddressType::PublicKey).public_key(), byteStream.data(),
                                   byteStream.size())) {
        cslog() << "API: reject transaction with wrong signature";
        _return.status.code = ERROR_CODE;
        _return.status.message = "wrong signature! ByteStream: " + cs::Utils::byteStreamToHex(fromByteArray(byteStream));
        return;
    }

    solver.send_wallet_transaction(tr);

    //
    /*const static unsigned int WAIT_SECONDS_TIME{ 10 };
    const auto pair = std::pair(tr.source(), tr.innerID());
    auto lockRef{ lockedReference(this->trxInProgress) };
    auto& cvTrxn = (*lockRef)[pair];
    static std::mutex mt;
    std::unique_lock lock(mt);
    const auto resWait = cvTrxn.first.wait_for(lock, std::chrono::seconds(WAIT_SECONDS_TIME), [&cvflg = (cvTrxn.second = false)]{ return cvflg; });
    lockedReference(this->trxInProgress)->erase(pair);
    //

    if (!resWait) // time is over
            SetResponseStatus(_return.status, APIRequestStatusType::INPROGRESS, get_delimited_transaction_sighex(tr));
    else*/
    SetResponseStatus(_return.status, APIRequestStatusType::SUCCESS, get_delimited_transaction_sighex(tr));
}

template <typename T>
std::enable_if<std::is_convertible<T*, ::apache::thrift::TBase*>::type, std::ostream&> operator<<(std::ostream& s, const T& t) {
    t.printTo(s);
    return s;
}

void APIHandler::smart_transaction_flow(api::TransactionFlowResult& _return, const Transaction& transaction) {
    auto input_smart = transaction.smartContract;
    auto send_transaction = make_transaction(transaction);
    const auto smart_addr = s_blockchain.getAddressByType(send_transaction.target(), BlockChain::AddressType::PublicKey);
    const bool deploy = is_smart_deploy(input_smart);

    send_transaction.add_user_field(cs::trx_uf::deploy::Code, serialize(transaction.smartContract));

    if (!input_smart.forgetNewState) {
        // check money
        const auto source_addr = s_blockchain.getAddressByType(send_transaction.source(), BlockChain::AddressType::PublicKey);
        BlockChain::WalletData wallData{};
        BlockChain::WalletId wallId{};
        if (!s_blockchain.findWalletData(source_addr, wallData, wallId)) {
            _return.status.code = ERROR_CODE;
            _return.status.message = "not enough money!";
            return;
        }

        const auto max_fee = send_transaction.max_fee().to_double();
        const auto balance = wallData.balance_.to_double();
        if (max_fee > balance) {
            _return.status.code = ERROR_CODE;
            _return.status.message = "not enough money!\nmax_fee: " + std::to_string(max_fee) + "\nbalance: " + std::to_string(balance);
            return;
        }
        //

        // check max fee
        {
            csdb::AmountCommission countedFee;
            if (!cs::fee::estimateMaxFee(send_transaction, countedFee)) {
                _return.status.code = ERROR_CODE;
                _return.status.message = "max fee is not enough, counted fee will be " + std::to_string(countedFee.to_double());
                return;
            }
        }

        // check signature
        const auto byteStream = send_transaction.to_byte_stream_for_sig();
        if (!cscrypto::verifySignature(send_transaction.signature(), s_blockchain.getAddressByType(send_transaction.source(), BlockChain::AddressType::PublicKey).public_key(),
                                       byteStream.data(), byteStream.size())) {
            _return.status.code = ERROR_CODE;
            cslog() << "API: reject transaction with wrong signature";
            _return.status.message = "wrong signature! ByteStream: " + cs::Utils::byteStreamToHex(fromByteArray(byteStream));
            return;
        }
        //
    }

    std::vector<general::ByteCodeObject> origin_bytecode;
    if (!deploy) {
        for (auto& it : input_smart.smartContractDeploy.byteCodeObjects) {
            it.byteCode.clear();
        }

        input_smart.smartContractDeploy.sourceCode.clear();

        decltype(auto) locked_smart_origin = lockedReference(this->smart_origin);
        auto it = locked_smart_origin->find(smart_addr);
        if (it != locked_smart_origin->end())
            origin_bytecode = fetch_smart(s_blockchain.loadTransaction(it->second)).smartContractDeploy.byteCodeObjects;
        else {
            SetResponseStatus(_return.status, APIRequestStatusType::FAILURE);
            return;
        }
    }
    else {
        csdb::Address addr = s_blockchain.getAddressByType(send_transaction.target(), BlockChain::AddressType::PublicKey);
        csdb::Address deployer = s_blockchain.getAddressByType(send_transaction.source(), BlockChain::AddressType::PublicKey);
        auto scKey = cs::SmartContracts::get_valid_smart_address(deployer, send_transaction.innerID(), input_smart.smartContractDeploy);
        if (scKey != addr) {
            _return.status.code = ERROR_CODE;
            const auto data = scKey.public_key().data();
            std::string str = EncodeBase58(data, data + cscrypto::kPublicKeySize);
            _return.status.message = "Bad smart contract address, expected " + str;
            return;
        }
    }

    auto& contract_state_entry = [this, &smart_addr]() -> decltype(auto) {
        auto smart_state(lockedReference(this->smart_state));
        return (*smart_state)[smart_addr];
    }();

    contract_state_entry.getPosition();

    if (input_smart.forgetNewState) {
        // -- prevent start transaction from flow to solver, so move it here:
        std::string contract_state;
        if (!deploy) {
            auto resWait = contract_state_entry.waitTillFront([&](SmartState& ss) {
                if (ss.state.empty())
                    return false;
                contract_state = ss.state;
                return true;
            });
            if (!resWait) {  // time is over
                SetResponseStatus(_return.status, APIRequestStatusType::INPROGRESS);
                return;
            }
        }
        // --
        auto source_pk = s_blockchain.getAddressByType(send_transaction.source(), BlockChain::AddressType::PublicKey);
        auto target_pk = s_blockchain.getAddressByType(send_transaction.target(), BlockChain::AddressType::PublicKey);
        executor::ExecuteByteCodeResult api_resp;
        const std::vector<general::ByteCodeObject>& bytecode = deploy ? input_smart.smartContractDeploy.byteCodeObjects : origin_bytecode;
        if (!deploy || !input_smart.smartContractDeploy.byteCodeObjects.empty()) {
            std::vector<executor::MethodHeader> methodHeader;
            {
                executor::MethodHeader tmp;
                tmp.methodName = input_smart.method;
                tmp.params = input_smart.params;
                methodHeader.push_back(tmp);
            }
            executor_.executeByteCode(api_resp, source_pk.to_api_addr(), target_pk.to_api_addr(), bytecode, contract_state, methodHeader, MAX_EXECUTION_TIME);
            if (api_resp.status.code) {
                _return.status.code = api_resp.status.code;
                _return.status.message = api_resp.status.message;
                contract_state_entry.yield();
                return;
            }
            _return.__isset.smart_contract_result = api_resp.__isset.results;
            if (_return.__isset.smart_contract_result && !api_resp.results.empty())
                _return.__set_smart_contract_result(api_resp.results[0].ret_val);
        }

        SetResponseStatus(_return.status, APIRequestStatusType::SUCCESS);
        contract_state_entry.yield();
        return;
    }

    solver.send_wallet_transaction(send_transaction);

    if (deploy) {
        auto resWait = contract_state_entry.waitTillFront([&](SmartState& ss) { return !ss.state.empty(); });
        if (!resWait) {  // time is over
            SetResponseStatus(_return.status, APIRequestStatusType::INPROGRESS);
            return;
        }
    }
    else {
        std::string new_state;
        csdb::TransactionID trId;

        auto resWait = contract_state_entry.waitTillFront([&](SmartState& ss) {
            auto execTrans = s_blockchain.loadTransaction(ss.initer);
            if (execTrans.is_valid() && execTrans.signature() == send_transaction.signature()) {
                new_state = ss.lastEmpty ? std::string() : ss.state;
                trId = ss.transaction.clone();
                return true;
            }
            return false;
        });

        if (!resWait) { // time is over
            SetResponseStatus(_return.status, APIRequestStatusType::INPROGRESS);
            return;
        }

        if (new_state.empty()) {
            _return.status.code = ERROR_CODE;
            _return.status.message = "state is not updated, execution failed";
            return;
        }
        else {
            auto stateTrans = s_blockchain.loadTransaction(trId);
            if (stateTrans.is_valid() && stateTrans.user_field_ids().count(cs::trx_uf::new_state::RetVal) > 0) {
                auto var_state = deserialize<::general::Variant>(stateTrans.user_field(cs::trx_uf::new_state::RetVal).value<std::string>());
                _return.__set_smart_contract_result(var_state);
            }
        }
    }
    SetResponseStatus(_return.status, APIRequestStatusType::SUCCESS, get_delimited_transaction_sighex(send_transaction));
}

void APIHandler::TransactionFlow(api::TransactionFlowResult& _return, const Transaction& transaction) {
    if (!transaction.__isset.smartContract) {
        dumb_transaction_flow(_return, transaction);
    }
    else {
        smart_transaction_flow(_return, transaction);
    }

    _return.roundNum = (uint32_t) cs::Conveyer::instance().currentRoundTable().round;
}

void APIHandler::PoolListGet(api::PoolListGetResult& _return, const int64_t offset, const int64_t const_limit) {
    if (!validatePagination(_return, *this, offset, const_limit)) {
        return;
    }

    uint64_t sequence = s_blockchain.getLastSequence();
    if ((uint64_t)offset > sequence) {
        return;
    }

    _return.pools.reserve(const_limit);

    csdb::PoolHash hash;
    try {
        hash = s_blockchain.getHashBySequence(sequence - offset);
    }
    catch (...) {
        return;
    }
    if (hash.is_empty()) {
        return;
    }
    PoolListGetStable(_return, fromByteArray(hash.to_binary()), const_limit);
    _return.count = uint32_t(sequence + 1);
}

void APIHandler::PoolTransactionsGet(PoolTransactionsGetResult& _return, const PoolHash& hash, const int64_t offset, const int64_t limit) {
    const csdb::PoolHash poolHash = csdb::PoolHash::from_binary(toByteArray(hash));
    csdb::Pool pool = s_blockchain.loadBlock(poolHash);

    if (pool.is_valid()) {
        _return.transactions = extractTransactions(pool, limit, offset);
    }

    SetResponseStatus(_return.status, APIRequestStatusType::SUCCESS);
}

void APIHandler::PoolInfoGet(PoolInfoGetResult& _return, const PoolHash& hash, const int64_t index) {
    csunused(index);
    const csdb::PoolHash poolHash = csdb::PoolHash::from_binary(toByteArray(hash));
    csdb::Pool pool = s_blockchain.loadBlock(poolHash);
    _return.isFound = pool.is_valid();

    if (_return.isFound) {
        _return.pool = convertPool(poolHash);
    }

    SetResponseStatus(_return.status, APIRequestStatusType::SUCCESS);
}

void APIHandler::StatsGet(api::StatsGetResult& _return) {
#ifdef MONITOR_NODE
    csstats::StatsPerPeriod stats_inst = this->stats.getStats();

    for (auto& s : stats_inst) {
        api::PeriodStats ps = {};
        ps.periodDuration = s.periodSec;
        ps.poolsCount = s.poolsCount;
        ps.transactionsCount = s.transactionsCount;
        ps.smartContractsCount = s.smartContractsCount;
        ps.transactionsSmartCount = s.transactionsSmartCount;

        for (auto& t : s.balancePerCurrency) {
            api::CumulativeAmount amount;
            amount.integral = t.second.integral;
            amount.fraction = t.second.fraction;
            ps.balancePerCurrency[t.first] = amount;
        }

        _return.stats.push_back(ps);
    }
#endif
    SetResponseStatus(_return.status, APIRequestStatusType::SUCCESS);
}

void APIHandler::SmartContractGet(api::SmartContractGetResult& _return, const general::Address& address) {
    auto smartrid = [&]() -> decltype(auto) {
        auto smart_origin = lockedReference(this->smart_origin);
        auto it = smart_origin->find(BlockChain::getAddressFromKey(address));
        return it == smart_origin->end() ? csdb::TransactionID() : it->second;
    }();
    if (!smartrid.is_valid()) {
        SetResponseStatus(_return.status, APIRequestStatusType::FAILURE);
        return;
    }
    _return.smartContract = fetch_smart_body(s_blockchain.loadTransaction(smartrid));
    const csdb::Address adrs = BlockChain::getAddressFromKey(address);
    auto locked_smart_state(lockedReference(this->smart_state));
    _return.smartContract.objectState = (*locked_smart_state)[adrs].getState().state;

    SetResponseStatus(_return.status, !_return.smartContract.address.empty() ? APIRequestStatusType::SUCCESS : APIRequestStatusType::FAILURE);
    return;
}

void APIHandler::store_block_slot(const csdb::Pool&) {
    newBlockCv_.notify_all();
}

void APIHandler::collect_all_stats_slot(const csdb::Pool& pool) {
    const ::csstats::Periods periods = ::csstats::collectionPeriods;

    static unsigned int currentCutIndex = 0;
    static auto startCutTime = stats_.first[currentCutIndex].timeStamp;
    static auto endCutTime = stats_.first[currentCutIndex + 1].timeStamp;

    auto now = std::chrono::system_clock::now();
    auto poolTime_t = atoll(pool.user_field(0).value<std::string>().c_str()) / 1000;
    auto poolTime = std::chrono::system_clock::from_time_t(poolTime_t);

    using Seconds = std::chrono::seconds;
    Seconds poolAgeSec = std::chrono::duration_cast<Seconds>(now - poolTime);

    if (startCutTime <= poolTime && poolTime < endCutTime) {
        ::csstats::PeriodStats& periodStats = stats_.first[currentCutIndex];
        ++periodStats.poolsCount;

        size_t transactionsCount = pool.transactions_count();
        periodStats.transactionsCount += (uint32_t)transactionsCount;

        for (size_t i = 0; i < transactionsCount; ++i) {
            const auto& transaction = pool.transaction(csdb::TransactionID(pool.hash(), i));

#ifdef MONITOR_NODE
            if (is_smart(transaction) || is_smart_state(transaction))
                ++periodStats.transactionsSmartCount;
#endif

            if (is_deploy_transaction(transaction)) {
                ++periodStats.smartContractsCount;
            }

            Currency currency = 1;

            const auto& amount = transaction.amount();

            periodStats.balancePerCurrency[currency].integral += amount.integral();
            periodStats.balancePerCurrency[currency].fraction += amount.fraction();
        }
    }
    else if ((currentCutIndex + 1) < stats_.first.size()) {
        startCutTime = stats_.first[currentCutIndex].timeStamp;
        endCutTime = stats_.first[currentCutIndex + 1].timeStamp;
        ++currentCutIndex;
    }

    auto period = (csstats::Period)poolAgeSec.count();
    for (size_t periodIndex = 0; periodIndex < periods.size(); ++periodIndex) {
        if (period < periods[periodIndex]) {
            csstats::PeriodStats& periodStats = stats_.second[periodIndex];
            periodStats.poolsCount++;

            size_t transactionsCount = pool.transactions_count();
            periodStats.transactionsCount += (uint32_t)transactionsCount;

            for (size_t i = 0; i < transactionsCount; ++i) {
                const auto& transaction = pool.transaction(csdb::TransactionID(pool.hash(), i));

                if (transaction.source() == s_blockchain.getGenesisAddress())
                    continue;
#ifdef MONITOR_NODE
                if (is_smart(transaction) || is_smart_state(transaction)) {
                    ++periodStats.transactionsSmartCount;
                }
#endif

                if (is_deploy_transaction(transaction)) {
                    ++periodStats.smartContractsCount;
                }

                //Currency currency = currencies_indexed[transaction.currency().to_string()];
                Currency currency = 1;

                const auto& amount = transaction.amount();

                periodStats.balancePerCurrency[currency].integral += amount.integral();
                periodStats.balancePerCurrency[currency].fraction += amount.fraction();
            }
        }
    }
}
//

void APIHandler::update_smart_caches_slot(const csdb::Pool& pool) {
    if (!pool.is_valid()) {
        return;
    }
    auto locked_pending_smart_transactions = lockedReference(this->pending_smart_transactions);
    locked_pending_smart_transactions->last_pull_hash = pool.hash();
    if (locked_pending_smart_transactions->last_pull_sequence < pool.sequence()) {
        locked_pending_smart_transactions->last_pull_sequence = pool.sequence();
    }
    auto& trs = pool.transactions();
    for (auto i_tr = trs.rbegin(); i_tr != trs.rend(); ++i_tr) {
        auto& tr = *i_tr;
        if (is_smart(tr) || is_smart_state(tr)) {
            locked_pending_smart_transactions->queue.push(std::make_pair(pool.sequence(), tr));
        }
    }

    if (!locked_pending_smart_transactions->queue.empty()) {
        auto elt = std::move(locked_pending_smart_transactions->queue.front());
        auto& tr = elt.second;
        locked_pending_smart_transactions->queue.pop();
        auto address = s_blockchain.getAddressByType(tr.target(), BlockChain::AddressType::PublicKey);

        auto source_pk = s_blockchain.getAddressByType(tr.source(), BlockChain::AddressType::PublicKey);
        auto target_pk = s_blockchain.getAddressByType(tr.target(), BlockChain::AddressType::PublicKey);

        if (is_smart_state(tr)) {
            cs::SmartContractRef scr;
            scr.from_user_field(tr.user_field(cs::trx_uf::new_state::RefStart));
            csdb::TransactionID trId(scr.hash, scr.transaction);

            std::string newState;
            auto locked_smart_state(lockedReference(this->smart_state));
            (*locked_smart_state)[address].updateState([&](const SmartState& oldState) {
                newState = tr.user_field(kSmartStateIndex).value<std::string>();
                return SmartState{newState.empty() ? oldState.state : newState, newState.empty(), tr.id().clone(), trId.clone()};
            });

            auto execTrans = s_blockchain.loadTransaction(trId);
            if (execTrans.is_valid() && is_smart(execTrans)) {
                const auto smart = fetch_smart(execTrans);
                if(!smart.method.empty()) {
                    mExecuteCount_[smart.method]++;
                }

                {
                    auto retVal = tr.user_field(cs::trx_uf::new_state::RetVal).value<std::string>();
                    ::general::Variant val;
                    if (!retVal.empty()) {
                        val = deserialize<::general::Variant>(std::move(retVal));
                    }

                    auto opers = lockedReference(this->smart_operations);
                    auto& op = (*opers)[trId];
                    op.state = newState.empty() ? SmartOperation::State::Failed : SmartOperation::State::Success;
                    op.stateTransaction = tr.id();

                    if (!retVal.empty()) {
                        op.hasRetval = true;
                        if (val.__isset.v_boolean || val.__isset.v_boolean_box) {
                            op.returnsBool = true;
                            op.boolResult = val.__isset.v_boolean ? val.v_boolean : val.v_boolean_box;
                        }
                    }
                }

                auto caller_pk = s_blockchain.getAddressByType(execTrans.source(), BlockChain::AddressType::PublicKey);

                if (is_smart_deploy(smart)) {
                    tm.checkNewDeploy(target_pk, caller_pk, smart);
                }

                newState = tr.user_field(kSmartStateIndex).value<std::string>();
                if (!newState.empty()) {
                    tm.checkNewState(target_pk, caller_pk, smart, newState);
                }
            }
        }
        else {
            const auto smart = fetch_smart(tr);

            {
                auto opers = lockedReference(this->smart_operations);
                (*opers)[tr.id()];

                auto sp = lockedReference(this->smarts_pending);
                (*sp)[elt.first].push_back(tr.id());
            }

            if (is_smart_deploy(smart)) {
                {
                    if (!smart.smartContractDeploy.byteCodeObjects.empty()) {
                        auto locked_smart_origin = lockedReference(this->smart_origin);
                        (*locked_smart_origin)[address] = tr.id().clone();

                        executor_.updateDeployTrxns(address, tr.id().clone());
                    }
                }
                {
                    auto locked_deployed_by_creator = lockedReference(this->deployed_by_creator);
                    (*locked_deployed_by_creator)[source_pk].push_back(tr.id().clone());
                }
            }
        }
    }
    else {
        auto sp = lockedReference(this->smarts_pending);
        auto so = lockedReference(this->smart_operations);
        for (auto it = sp->begin(); it != sp->end(); it = sp->erase(it)) {
            if ((it->first + Consensus::MaxRoundsCancelContract) > locked_pending_smart_transactions->last_pull_sequence) {
                break;
            }

            for (auto& sm : it->second) {
                auto& oper = (*so)[sm];
                if (oper.state == SmartOperation::State::Pending) {
                    oper.state = SmartOperation::State::Failed;
                }
            }
        }
    }
}

bool APIHandler::update_smart_caches_once(const csdb::PoolHash& start, bool init) {
    auto locked_pending_smart_transactions = lockedReference(this->pending_smart_transactions);
    std::vector<csdb::PoolHash> new_blocks;
    auto curph = start;

    static bool log_to_console = true;
    size_t cnt = 0;

    if (log_to_console) {
        std::cout << "API: analizing blockchain...\n";
    }
    while (curph != locked_pending_smart_transactions->last_pull_hash) {
        ++cnt;
        new_blocks.push_back(curph);
        size_t res;
        auto p = s_blockchain.loadBlockMeta(curph, res);
        curph = p.previous_hash();
        if (log_to_console && (cnt % 1000) == 0) {
            std::cout << '\r' << WithDelimiters(cnt);
        }
        // temp workaround of strange issue in recursive_mutex (BlockCHoain::DbLock_)
        if ((cnt % 100) == 0) {
            std::this_thread::yield();
        }
        if (p.is_valid() && locked_pending_smart_transactions->last_pull_sequence < p.sequence()) {
            locked_pending_smart_transactions->last_pull_sequence = p.sequence();
        }

        if (curph.is_empty()) {
            if (log_to_console) {
                std::cout << '\r' << WithDelimiters(cnt) << "... Done\n";
            }
            break;
        }
    }

    if (curph.is_empty() && !locked_pending_smart_transactions->last_pull_hash.is_empty()) {
        // Fork detected!
        cnt = 0;
        if (log_to_console) {
            std::cout << "API: fork detected, handling " << WithDelimiters(new_blocks.size()) << " hashes...\n";
        }
        auto luca = locked_pending_smart_transactions->last_pull_hash;
        while (!luca.is_empty()) {
            ++cnt;
            auto fIt = std::find(new_blocks.begin(), new_blocks.end(), luca);
            if (fIt != new_blocks.end()) {
                new_blocks.erase(fIt, new_blocks.end());
                break;
            }
            if (log_to_console && (cnt % 100) == 0) {
                std::cout << '\r' << WithDelimiters(cnt);
            }
            size_t res;
            luca = s_blockchain.loadBlockMeta(luca, res).previous_hash();
            // temp workaround of strange issue in recursive_mutex (BlockCHoain::DbLock_)
            if ((cnt % 100) == 0) {
                std::this_thread::yield();
            }
        }
        if (log_to_console) {
            std::cout << '\r' << WithDelimiters(cnt) << "... Done\n";
        }
    }

    locked_pending_smart_transactions->last_pull_hash = start;

    cnt = 0;
    if (log_to_console) {
        std::cout << "API: searching for smart states in " << WithDelimiters(new_blocks.size()) << " blocks...\n";
    }
    while (!new_blocks.empty()) {
        ++cnt;
        auto p = s_blockchain.loadBlock(new_blocks.back());
        new_blocks.pop_back();
        auto& trs = p.transactions();
        for (auto i_tr = trs.rbegin(); i_tr != trs.rend(); ++i_tr) {
            auto& tr = *i_tr;
            if (is_smart(tr) || is_smart_state(tr)) {
                locked_pending_smart_transactions->queue.push(std::make_pair(p.sequence(), tr));
            }
            /*else { // simple transactions
                auto lockRef{ lockedReference(this->trxInProgress) };
                if (auto it = lockRef->find(std::pair(tr.source(), tr.innerID())); it != lockRef->end()) {
                    (it->second).second = true;
                    (it->second).first.notify_all();
                }
            }*/
        }
        if (log_to_console && (cnt % 1000) == 0) {
            std::cout << '\r' << WithDelimiters(cnt);
        }
    }
    if (log_to_console) {
        std::cout << "\rDone, handled " << WithDelimiters(cnt) << " blocks...\n";
    }
    log_to_console = false;

    if (!locked_pending_smart_transactions->queue.empty()) {
        auto elt = std::move(locked_pending_smart_transactions->queue.front());
        auto& tr = elt.second;
        locked_pending_smart_transactions->queue.pop();
        auto address = s_blockchain.getAddressByType(tr.target(), BlockChain::AddressType::PublicKey);

        auto source_pk = s_blockchain.getAddressByType(tr.source(), BlockChain::AddressType::PublicKey);
        auto target_pk = s_blockchain.getAddressByType(tr.target(), BlockChain::AddressType::PublicKey);

        if (is_smart_state(tr)) {
            cs::SmartContractRef scr;
            scr.from_user_field(tr.user_field(cs::trx_uf::new_state::RefStart));
            csdb::TransactionID trId(scr.hash, scr.transaction);

            std::string newState;
            auto locked_smart_state(lockedReference(this->smart_state));
            (*locked_smart_state)[address].updateState([&](const SmartState& oldState) {
                newState = tr.user_field(kSmartStateIndex).value<std::string>();
                return SmartState{newState.empty() ? oldState.state : newState, newState.empty(), tr.id().clone(), trId.clone()};
            });

            auto execTrans = s_blockchain.loadTransaction(trId);

            if ((execTrans.is_valid() && is_smart(execTrans)) || 
                    execTrans.amount().to_double()) { // payable
                const auto smart = fetch_smart(execTrans);

                if (!smart.method.empty()) {
                    mExecuteCount_[smart.method]++;
                }

                {
                    auto retVal = tr.user_field(cs::trx_uf::new_state::RetVal).value<std::string>();
                    ::general::Variant val;
                    if (!retVal.empty()) {
                        val = deserialize<::general::Variant>(std::move(retVal));
                    }

                    auto opers = lockedReference(this->smart_operations);
                    auto& op = (*opers)[trId];
                    op.state = newState.empty() ? SmartOperation::State::Failed : SmartOperation::State::Success;
                    op.stateTransaction = tr.id();

                    if (!retVal.empty()) {
                        op.hasRetval = true;
                        if (val.__isset.v_boolean || val.__isset.v_boolean_box) {
                            op.returnsBool = true;
                            op.boolResult = val.__isset.v_boolean ? val.v_boolean : val.v_boolean_box;
                        }
                    }
                }

                auto caller_pk = s_blockchain.getAddressByType(execTrans.source(), BlockChain::AddressType::PublicKey);

                if (is_smart_deploy(smart)) {
                    tm.checkNewDeploy(target_pk, caller_pk, smart);
                }

                newState = tr.user_field(kSmartStateIndex).value<std::string>();
                if (!newState.empty()) {
                    tm.checkNewState(target_pk, caller_pk, smart, newState);
                }
            }
        }
        else {
            const auto smart = fetch_smart(tr);
            if (!init) {
                auto& e = [&]() -> decltype(auto) {
                    auto smart_last_trxn = lockedReference(this->smart_last_trxn);
                    return (*smart_last_trxn)[address];
                }();
                std::unique_lock lock(e.lock);
                e.trid_queue.push_back(tr.id().clone());
                e.new_trxn_cv.notify_all();
            }

            {
                auto opers = lockedReference(this->smart_operations);
                (*opers)[tr.id()];

                auto sp = lockedReference(this->smarts_pending);
                (*sp)[elt.first].push_back(tr.id());
            }

            if (is_smart_deploy(smart)) {
                {
                    if (!smart.smartContractDeploy.byteCodeObjects.empty()) {
                        auto locked_smart_origin = lockedReference(this->smart_origin);
                        (*locked_smart_origin)[address] = tr.id().clone();

                        executor_.updateDeployTrxns(address, tr.id().clone());
                    }
                }
                {
                    auto locked_deployed_by_creator = lockedReference(this->deployed_by_creator);
                    (*locked_deployed_by_creator)[source_pk].push_back(tr.id().clone());
                }
            }

            return true;
        }
    }
    else {
        auto sp = lockedReference(this->smarts_pending);
        auto so = lockedReference(this->smart_operations);
        for (auto it = sp->begin(); it != sp->end(); it = sp->erase(it)) {
            if ((it->first + Consensus::MaxRoundsCancelContract) > locked_pending_smart_transactions->last_pull_sequence) {
                break;
            }

            for (auto& sm : it->second) {
                auto& oper = (*so)[sm];
                if (oper.state == SmartOperation::State::Pending) {
                    oper.state = SmartOperation::State::Failed;
                }
            }
        }
    }

    return false;
}

template <typename Mapper>
size_t APIHandler::getMappedDeployerSmart(const csdb::Address& deployer, Mapper mapper, std::vector<decltype(mapper(api::SmartContract()))>& out) {
    auto locked_deployed_by_creator = lockedReference(this->deployed_by_creator);
    auto& elt = (*locked_deployed_by_creator)[deployer];
    for (auto& trid : elt) {
        auto tr = s_blockchain.loadTransaction(trid);
        auto smart = fetch_smart_body(tr);
        out.push_back(mapper(smart));
    }

    return elt.size();
}

void APIHandler::SmartContractsListGet(api::SmartContractsListGetResult& _return, const general::Address& deployer) {
    const csdb::Address addr = BlockChain::getAddressFromKey(deployer);

    _return.count = static_cast<decltype(_return.count)>(getMappedDeployerSmart(addr, [](const api::SmartContract& smart) {
        return smart;
    }, _return.smartContractsList));

    SetResponseStatus(_return.status, APIRequestStatusType::SUCCESS);
}

void APIHandler::SmartContractAddressesListGet(api::SmartContractAddressesListGetResult& _return, const general::Address& deployer) {
    const csdb::Address addr = BlockChain::getAddressFromKey(deployer);

    getMappedDeployerSmart(addr, [](const SmartContract& sc) {
        return sc.address;
    }, _return.addressesList);

    SetResponseStatus(_return.status, APIRequestStatusType::SUCCESS);
}

void APIHandler::GetLastHash(api::PoolHash& _return) {
    _return = fromByteArray(s_blockchain.getLastHash().to_binary());
    return;
}

void APIHandler::PoolListGetStable(api::PoolListGetResult& _return, const api::PoolHash& api_hash, const int64_t const_limit) {
    if (const_limit <= 0 || const_limit > 100) {
        SetResponseStatus(_return.status, APIRequestStatusType::FAILURE);
        return;
    }

    auto hash = csdb::PoolHash::from_binary(toByteArray(api_hash));
    auto limit = const_limit;

    bool limSet = false;

    while (limit && !hash.is_empty()) {
        auto cch = poolCache.find(hash);

        if (cch == poolCache.end()) {
            auto pool = s_blockchain.loadBlock(hash);
            api::Pool apiPool = convertPool(pool);
            _return.pools.push_back(apiPool);
            poolCache.insert(cch, std::make_pair(hash, apiPool));
            hash = pool.previous_hash();

            if (!limSet) {
                _return.count = uint32_t(pool.sequence() + 1);
                limSet = true;
            }
        }
        else {
            _return.pools.push_back(cch->second);
            hash = csdb::PoolHash::from_binary(toByteArray(cch->second.prevHash));

            if (!limSet) {
                _return.count = uint32_t(cch->second.poolNumber + 1);
                limSet = true;
            }
        }

        --limit;
    }
}

void APIHandler::WaitForSmartTransaction(api::TransactionId& _return, const general::Address& smart_public) {
    csdb::Address key = BlockChain::getAddressFromKey(smart_public);
    decltype(smart_last_trxn)::LockedType::iterator it;
    auto& entry = [&]() -> decltype(auto) {
        auto smart_last_trxn = lockedReference(this->smart_last_trxn);
        std::tie(it, std::ignore) = smart_last_trxn->emplace(std::piecewise_construct, std::forward_as_tuple(key), std::forward_as_tuple());
        return std::ref(it->second).get();
    }();

    {
        std::unique_lock<decltype(entry.lock)> l(entry.lock);
        ++entry.awaiter_num;
        const auto checker = [&]() {
            if (!entry.trid_queue.empty()) {
                _return = convert_transaction_id(entry.trid_queue.front());
                if (--entry.awaiter_num == 0) {
                    entry.trid_queue.pop_front();
                }
                return true;
            }
            return false;
        };
        entry.new_trxn_cv.wait(l, checker);
    }
}

void APIHandler::SmartContractsAllListGet(SmartContractsListGetResult& _return, const int64_t _offset, const int64_t _limit) {
    int64_t offset = _offset;
    int64_t limit = _limit;

    auto locked_smart_origin = lockedReference(this->smart_origin);

    _return.count = (uint32_t) locked_smart_origin->size();

    for (auto p : *locked_smart_origin) {
        if (offset) {
            --offset;
        }
        else if (limit) {
            auto trid = p.second;
            auto tr = s_blockchain.loadTransaction(trid);
            _return.smartContractsList.push_back(fetch_smart_body(tr));
            --limit;
        }
        else {
            break;
        }
    }

    SetResponseStatus(_return.status, APIRequestStatusType::SUCCESS);
}

void api::APIHandler::WaitForBlock(PoolHash& _return, const PoolHash& /* obsolete */) {
    std::unique_lock lock(dbLock_);
    newBlockCv_.wait(lock);
    _return = fromByteArray(s_blockchain.getLastHash().to_binary());
}

void APIHandler::TransactionsStateGet(TransactionsStateGetResult& _return, const general::Address& address, const std::vector<int64_t>& v) {
    const csdb::Address addr = BlockChain::getAddressFromKey(address);
    for (auto inner_id : v) {
        csdb::Transaction transactionTmp;
        BlockChain::WalletData wallData{};
        BlockChain::WalletId wallId{};
        inner_id &= 0x3fffffffffff;
        bool finish_for_idx = false;
        if (!s_blockchain.findWalletData(addr, wallData, wallId)) {
            SetResponseStatus(_return.status, APIRequestStatusType::NOT_FOUND);
            return;
        }
        auto addr_id = csdb::Address::from_wallet_id(wallId);
        if (s_blockchain.getTransaction(addr_id, inner_id, transactionTmp)) {// find in blockchain
            _return.states[inner_id] = VALID;
        }
        else {
            cs::Conveyer& conveyer = cs::Conveyer::instance();
            auto lock = conveyer.lock();
            for (decltype(auto) it : conveyer.packetQueue()) {
                const auto& transactions = it.transactions();
                for (decltype(auto) transaction : transactions) {
                    if (transaction.innerID() == inner_id) {
                        _return.states[inner_id] = INPROGRESS;
                        finish_for_idx = true;
                        break;
                    }
                }
            }
            if (!finish_for_idx) {
                decltype(auto) m_hash_tb = conveyer.transactionsPacketTable();  // find in hash table
                for (decltype(auto) it : m_hash_tb) {
                    const auto& transactions = it.second.transactions();
                    for (decltype(auto) transaction : transactions) {
                        if (transaction.innerID() == inner_id) {
                            _return.states[inner_id] = INPROGRESS;
                            finish_for_idx = true;
                            break;
                        }
                    }
                }
            }
            lock.unlock();
            if (!finish_for_idx) {                                // if hash table doesn't contain trx return true if in last 5 rounds
                if (conveyer.isMetaTransactionInvalid(inner_id)) {// trx is invalid (time between del from hash table and add to blockchain)
                    _return.states[inner_id] = INVALID;
                }
                else {
                    _return.states[inner_id] = VALID;
                }
            }
        }
    }
    _return.roundNum = (uint32_t) cs::Conveyer::instance().currentRoundTable().round;
    SetResponseStatus(_return.status, APIRequestStatusType::SUCCESS);
}

void api::APIHandler::SmartMethodParamsGet(SmartMethodParamsGetResult& _return, const general::Address& address, const int64_t id) {
    csdb::Transaction trx;
    const csdb::Address addr = BlockChain::getAddressFromKey(address);
    if (!s_blockchain.getTransaction(addr, id, trx)) {
        SetResponseStatus(_return.status, APIRequestStatusType::FAILURE);
        return;
    }
    _return.method = convertTransaction(trx).trxn.smartContract.method;
    _return.params = convertTransaction(trx).trxn.smartContract.params;
    SetResponseStatus(_return.status, APIRequestStatusType::SUCCESS);
}

void APIHandler::ContractAllMethodsGet(ContractAllMethodsGetResult& _return, const std::vector<general::ByteCodeObject>& byteCodeObjects) {
    executor::GetContractMethodsResult executor_ret;

    if (byteCodeObjects.empty()) {
        return;
    }

    executor_.getContractMethods(executor_ret, byteCodeObjects);
    _return.code = executor_ret.status.code;
    _return.message = executor_ret.status.message;

    for (size_t Count = 0; Count < executor_ret.methods.size(); Count++) {
        _return.methods[Count].name = executor_ret.methods[Count].name;

        for (size_t SubCount = 0; SubCount < executor_ret.methods[Count].arguments.size(); SubCount++) {
            _return.methods[Count].arguments[SubCount].type = executor_ret.methods[Count].arguments[SubCount].type;
            _return.methods[Count].arguments[SubCount].name = executor_ret.methods[Count].arguments[SubCount].name;
        }

        _return.methods[Count].returnType = executor_ret.methods[Count].returnType;
    }
}

////////new
void addTokenResult(api::TokenTransfersResult& _return, const csdb::Address& token, const std::string& code, const csdb::Pool& pool, const csdb::Transaction& tr,
                    const api::SmartContractInvocation& smart, const std::pair<csdb::Address, csdb::Address>& addrPair, const BlockChain& handler) {
    api::TokenTransfer transfer;
    transfer.token = fromByteArray(token.public_key());
    transfer.code = code;
    transfer.sender = fromByteArray(addrPair.first.public_key());
    transfer.receiver = fromByteArray(addrPair.second.public_key());
    transfer.amount = TokensMaster::getAmount(smart);
    transfer.initiator = fromByteArray(handler.getAddressByType(tr.source(), BlockChain::AddressType::PublicKey).public_key());

    transfer.transaction.poolHash = fromByteArray(tr.id().pool_hash().to_binary());
    transfer.transaction.index = (uint32_t) tr.id().index();
    transfer.time = atoll(pool.user_field(0).value<std::string>().c_str());
    _return.transfers.push_back(transfer);
}

void addTokenResult(api::TokenTransactionsResult& _return, const csdb::Address& token, const std::string&, const csdb::Pool& pool, const csdb::Transaction& tr,
                    const api::SmartContractInvocation& smart, const std::pair<csdb::Address, csdb::Address>&, BlockChain& handler) {
    api::TokenTransaction trans;
    trans.token = fromByteArray(token.public_key());
    trans.transaction.poolHash = fromByteArray(tr.id().pool_hash().to_binary());
    trans.transaction.index = (uint32_t) tr.id().index();
    trans.time = atoll(pool.user_field(0).value<std::string>().c_str());
    trans.initiator = fromByteArray(handler.getAddressByType(tr.source(), BlockChain::AddressType::PublicKey).public_key());
    trans.method = smart.method;
    trans.params = smart.params;
    _return.transactions.push_back(trans);
}

void putTokenInfo(api::TokenInfo& ti, const general::Address& addr, const Token& token) {
    ti.address = addr;
    ti.code = token.symbol;
    ti.name = token.name;
    ti.totalSupply = token.totalSupply;
    ti.owner = fromByteArray(token.owner.public_key());
    ti.transfersCount = (uint32_t) token.transfersCount;
    ti.transactionsCount = (uint32_t) token.transactionsCount;
    ti.holdersCount = (uint32_t) token.realHoldersCount;
    ti.standart = (decltype(api::TokenInfo::standart))(uint32_t)(token.standart);
}

template <typename ResultType>
void tokenTransactionsInternal(ResultType& _return, APIHandler& handler, TokensMaster& tm, const general::Address& token, bool transfersOnly, bool filterByWallet, int64_t offset,
                               int64_t limit, const csdb::Address& wallet = csdb::Address()) {
    if (!validatePagination(_return, handler, offset, limit)) {
        return;
    }

    const csdb::Address addr = BlockChain::getAddressFromKey(token);
    bool tokenFound = false;
    std::string code;

    tm.applyToInternal([&addr, &tokenFound, &transfersOnly, &filterByWallet, &code, &wallet, &_return](const TokensMap& tm, const HoldersMap&) {
        auto it = tm.find(addr);
        tokenFound = !(it == tm.end());
        if (tokenFound) {
            code = it->second.symbol;
            if (transfersOnly && !filterByWallet) {
                _return.count = it->second.transfersCount;
            }
            else if (!transfersOnly) {
                _return.count = it->second.transactionsCount;
            }
            else if (transfersOnly && filterByWallet) {
                auto hIt = it->second.holders.find(wallet);
                if (hIt != it->second.holders.end()) {
                    _return.count = hIt->second.transfersCount;
                }
                else {
                    _return.count = 0;
                }
            }
        }
    });

    if (!tokenFound) {
        handler.SetResponseStatus(_return.status, APIHandlerBase::APIRequestStatusType::FAILURE);
        return;
    }

    handler.iterateOverTokenTransactions(addr, [&_return, &offset, &limit, &addr, &code, &transfersOnly, &filterByWallet, &wallet, &s_blockchain = handler.get_s_blockchain()](
                                                   const csdb::Pool& pool, const csdb::Transaction& tr) {
        auto smart = fetch_smart(tr);
        if (transfersOnly && !TokensMaster::isTransfer(smart.method, smart.params)) {
            return true;
        }

        csdb::Address addr_pk = s_blockchain.getAddressByType(tr.source(), BlockChain::AddressType::PublicKey);
        auto addrPair = TokensMaster::getTransferData(addr_pk, smart.method, smart.params);

        if (filterByWallet && addrPair.first != wallet && addrPair.second != wallet) {
            return true;
        }

        if (--offset >= 0) {
            return true;
        }

        addTokenResult(_return, addr, code, pool, tr, smart, addrPair, s_blockchain);
        return !(--limit == 0);
    });

    handler.SetResponseStatus(_return.status, APIHandlerBase::APIRequestStatusType::SUCCESS);
}

void APIHandler::iterateOverTokenTransactions(const csdb::Address& addr, const std::function<bool(const csdb::Pool&, const csdb::Transaction&)> func) {
    std::list<csdb::TransactionID> l_id;
    for (auto trIt = TransactionsIterator(s_blockchain, addr); trIt.isValid(); trIt.next()) {
        if (is_smart_state(*trIt)) {
            cs::SmartContractRef smart_ref;
            smart_ref.from_user_field(trIt->user_field(cs::trx_uf::new_state::RefStart));
            l_id.emplace_back(csdb::TransactionID(smart_ref.hash, smart_ref.transaction));
        }
        else if (is_smart(*trIt)) {
            auto it = std::find(l_id.begin(), l_id.end(), trIt->id());
            if (it != l_id.end()) {
                l_id.erase(it);
                if (!func(trIt.getPool(), *trIt)) {
                    break;
                }
            }
        }
    }
}

////////new
api::SmartContractInvocation APIHandler::getSmartContract(const csdb::Address& addr, bool& present) {
    csdb::Address abs_addr = addr;
    if (addr.is_wallet_id()) {
        abs_addr = s_blockchain.getAddressByType(addr, BlockChain::AddressType::PublicKey);
    }

    decltype(auto) locked_smart_origin = lockedReference(this->smart_origin);

    auto it = locked_smart_origin->find(abs_addr);
    if ((present = (it != locked_smart_origin->end()))) {
        return fetch_smart(s_blockchain.loadTransaction(it->second));
    }
    return api::SmartContractInvocation{};
}

std::vector<general::ByteCodeObject> APIHandler::getSmartByteCode(const csdb::Address& addr, bool& present) {
    auto invocation = getSmartContract(addr, present);
    return present ? invocation.smartContractDeploy.byteCodeObjects : std::vector<general::ByteCodeObject>{};
}

void APIHandler::SmartContractCompile(api::SmartContractCompileResult& _return, const std::string& sourceCode) {
    executor::CompileSourceCodeResult result;
    executor_.compileSourceCode(result, sourceCode);

    if (result.status.code) {
        _return.status.code = result.status.code;
        _return.status.message = result.status.message;
        return;
    }

    executor::GetContractMethodsResult methodsResult;
    if (result.byteCodeObjects.empty()) {
        return;
    }
    executor_.getContractMethods(methodsResult, result.byteCodeObjects);

    if (methodsResult.status.code) {
        _return.status.code = methodsResult.status.code;
        _return.status.message = methodsResult.status.message;
        return;
    }

    _return.ts = (api::TokenStandart)(uint32_t)TokensMaster::getTokenStandart(methodsResult.methods);
    _return.byteCodeObjects = std::move(result.byteCodeObjects);

    SetResponseStatus(_return.status, APIRequestStatusType::SUCCESS);
}

void APIHandler::SmartContractDataGet(api::SmartContractDataResult& _return, const general::Address& address) {
    const csdb::Address addr = BlockChain::getAddressFromKey(address);

    bool present = false;
    std::vector<general::ByteCodeObject> byteCode = getSmartByteCode(addr, present);
    std::string state;

    {
        auto smartStateRef = lockedReference(this->smart_state);
        auto it = smartStateRef->find(addr);

        // if (it != smartStateRef->end()) state = it->second.get_current_state().state;
        // else present = false;

        if (it != smartStateRef->end())
            state = (*smartStateRef)[addr].getState().state;
        else
            present = false;
    }

    if (!present) {
        SetResponseStatus(_return.status, APIRequestStatusType::FAILURE);
        return;
    }

    executor::GetContractMethodsResult methodsResult;
    if (byteCode.empty())
        return;
    executor_.getContractMethods(methodsResult, byteCode);

    if (methodsResult.status.code) {
        _return.status.code = methodsResult.status.code;
        _return.status.message = methodsResult.status.message;
        return;
    }

    executor::GetContractVariablesResult variablesResult;
    if (byteCode.empty())
        return;
    executor_.getContractVariables(variablesResult, byteCode, state);

    if (variablesResult.status.code) {
        _return.status.code = variablesResult.status.code;
        _return.status.message = variablesResult.status.message;
        return;
    }

    for (auto& m : methodsResult.methods) {
        api::SmartContractMethod scm;
        scm.returnType = std::move(m.returnType);
        scm.name = std::move(m.name);
        for (auto& at : m.arguments) {
            api::SmartContractMethodArgument scma;

            scma.type = at.type;
            scma.name = at.name;

            scm.arguments.push_back(scma);
        }

        _return.methods.push_back(scm);
    }

    _return.variables = variablesResult.contractVariables;
    SetResponseStatus(_return.status, APIRequestStatusType::SUCCESS);
}

void APIHandler::ExecuteCountGet(ExecuteCountGetResult& _return, const std::string& executeMethod) {
    if (auto itCount = mExecuteCount_.find(executeMethod); itCount != mExecuteCount_.end()) {
        _return.executeCount = itCount->second;
        SetResponseStatus(_return.status, APIRequestStatusType::SUCCESS);
    }
    else {
        SetResponseStatus(_return.status, APIRequestStatusType::NOT_FOUND);
    }
}

void APIHandler::TokenBalancesGet(api::TokenBalancesResult& _return, const general::Address& address) {
    const csdb::Address addr = BlockChain::getAddressFromKey(address);
    tm.applyToInternal([&_return, &addr](const TokensMap& tokens, const HoldersMap& holders) {
        auto holderIt = holders.find(addr);
        if (holderIt != holders.end()) {
            for (const auto& tokAddr : holderIt->second) {
                auto tokenIt = tokens.find(tokAddr);
                if (tokenIt == tokens.end()) {
                    continue;  // This shouldn't happen
                }

                api::TokenBalance tb;
                tb.token = fromByteArray(tokenIt->first.public_key());
                tb.code = tokenIt->second.symbol;
                tb.name = tokenIt->second.name;

                auto hi = tokenIt->second.holders.find(addr);
                if (hi != tokenIt->second.holders.end()) {
                    tb.balance = hi->second.balance;
                }

                if (!TokensMaster::isZeroAmount(tb.balance)) {
                    _return.balances.push_back(tb);
                }
            }
        }
    });

    SetResponseStatus(_return.status, APIRequestStatusType::SUCCESS);
}

void APIHandler::TokenTransfersGet(api::TokenTransfersResult& _return, const general::Address& token, int64_t offset, int64_t limit) {
    tokenTransactionsInternal(_return, *this, tm, token, true, false, offset, limit);
}

void APIHandler::TokenTransferGet(api::TokenTransfersResult& _return, const general::Address& token, const TransactionId& id) {
    const csdb::PoolHash poolhash = csdb::PoolHash::from_binary(toByteArray(id.poolHash));
    const csdb::TransactionID trxn_id = csdb::TransactionID(poolhash, id.index);
    const csdb::Transaction trxn = s_blockchain.loadTransaction(trxn_id);
    const csdb::Address addr = BlockChain::getAddressFromKey(token);

    std::string code{};
    tm.applyToInternal([&addr, &code](const TokensMap& tm, const HoldersMap&) {
        const auto it = tm.find(addr);
        if (it != tm.cend()) {
            code = it->second.symbol;
        }
    });

    if (code.empty()) {
        SetResponseStatus(_return.status, APIRequestStatusType::FAILURE);
        return;
    }

    const auto pool = s_blockchain.loadBlock(trxn.id().pool_hash());
    const auto smart = fetch_smart(trxn);
    const auto addr_pk = s_blockchain.getAddressByType(trxn.source(), BlockChain::AddressType::PublicKey);
    const auto addrPair = TokensMaster::getTransferData(addr_pk, smart.method, smart.params);

    _return.count = 1;

    addTokenResult(_return, addr, code, pool, trxn, smart, addrPair, s_blockchain);
    SetResponseStatus(_return.status, APIRequestStatusType::SUCCESS);
}

#ifdef TRANSACTIONS_INDEX
void APIHandler::TransactionsListGet(api::TransactionsGetResult& _return, int64_t offset, int64_t limit) {
    if (!validatePagination(_return, *this, offset, limit))
        return;

    _return.result = false;
    _return.total_trxns_count = (uint32_t) s_blockchain.getTransactionsCount();

    auto tPair = s_blockchain.getLastNonEmptyBlock();
    while (limit > 0 && tPair.second) {
        if (tPair.second <= offset)
            offset -= tPair.second;
        else {
            auto p = s_blockchain.loadBlock(tPair.first);
            auto it = p.transactions().rbegin() + offset;
            offset = 0;

            while (it != p.transactions().rend() && limit > 0) {
                it->set_time(p.get_time());
                _return.transactions.push_back(convertTransaction(*it));
                _return.result = true;
                ++it;
                --limit;
            }
        }

        if (limit) {
            tPair = s_blockchain.getPreviousNonEmptyBlock(tPair.first);
        }
    }

    SetResponseStatus(_return.status, APIRequestStatusType::SUCCESS);
}

void APIHandler::TokenTransfersListGet(api::TokenTransfersResult& _return, int64_t offset, int64_t limit) {
    if (!validatePagination(_return, *this, offset, limit)) {
        return;
    }

    uint64_t totalTransfers = 0;
    std::map<csdb::Address, std::string> tokenCodes;
    std::multimap<csdb::PoolHash, csdb::Address> tokenTransPools;

    tm.applyToInternal([&totalTransfers, &tokenCodes, &tokenTransPools, this](const TokensMap& tm, const HoldersMap&) {
        for (auto& t : tm) {
            totalTransfers += t.second.transfersCount;
            tokenCodes[t.first] = t.second.symbol;
            tokenTransPools.insert(std::make_pair(s_blockchain.getLastTransaction(t.first).pool_hash(), t.first));
        }
    });

    _return.count = uint32_t(totalTransfers);

    csdb::PoolHash pooh = s_blockchain.getLastNonEmptyBlock().first;
    while (limit && !pooh.is_empty() && tokenTransPools.size()) {
        auto it = tokenTransPools.find(pooh);
        if (it != tokenTransPools.end()) {
            auto pool = s_blockchain.loadBlock(pooh);

            for (auto& t : pool.transactions()) {
                if (!is_smart(t)) {
                    continue;
                }
                auto tIt = tokenCodes.find(s_blockchain.getAddressByType(t.target(), BlockChain::AddressType::PublicKey));
                if (tIt == tokenCodes.end()) {
                    continue;
                }
                const auto smart = fetch_smart(t);
                if (!TokensMaster::isTransfer(smart.method, smart.params)) {
                    continue;
                }
                if (--offset >= 0) {
                    continue;
                }
                csdb::Address target_pk = s_blockchain.getAddressByType(t.target(), BlockChain::AddressType::PublicKey);
                auto addrPair = TokensMaster::getTransferData(target_pk, smart.method, smart.params);
                addTokenResult(_return, target_pk, tIt->second, pool, t, smart, addrPair, s_blockchain);
                if (--limit == 0) {
                    break;
                }
            }

            do {
                const auto lPh = s_blockchain.getPreviousPoolHash(it->second, it->first);
                const auto lAddr = it->second;

                tokenTransPools.erase(it);
                if (!lPh.is_empty()) {
                    tokenTransPools.insert(std::make_pair(lPh, lAddr));
                }

                it = tokenTransPools.find(pooh);
            } while (it != tokenTransPools.end());
        }

        pooh = s_blockchain.getPreviousNonEmptyBlock(pooh).first;
    }

    SetResponseStatus(_return.status, APIRequestStatusType::SUCCESS);
}

#endif

void APIHandler::TokenWalletTransfersGet(api::TokenTransfersResult& _return, const general::Address& token, const general::Address& address, int64_t offset, int64_t limit) {
    const csdb::Address wallet = BlockChain::getAddressFromKey(address);
    tokenTransactionsInternal(_return, *this, tm, token, true, true, offset, limit, wallet);
}

void APIHandler::TokenTransactionsGet(api::TokenTransactionsResult& _return, const general::Address& token, int64_t offset, int64_t limit) {
    tokenTransactionsInternal(_return, *this, tm, token, false, false, offset, limit);
}

void APIHandler::TokenInfoGet(api::TokenInfoResult& _return, const general::Address& token) {
    bool found = false;

    const csdb::Address addr = BlockChain::getAddressFromKey(token);
    tm.applyToInternal([&token, &addr, &found, &_return](const TokensMap& tm, const HoldersMap&) {
        auto tIt = tm.find(addr);
        if (tIt != tm.end()) {
            found = true;
            putTokenInfo(_return.token, token, tIt->second);
        }
    });

    SetResponseStatus(_return.status, found ? APIRequestStatusType::SUCCESS : APIRequestStatusType::FAILURE);
}

template <typename MapType, typename ComparatorType, typename FuncType>
static void applyToSortedMap(const MapType& map, const ComparatorType comparator, const FuncType func) {
    std::multiset<typename MapType::const_iterator, std::function<bool(const typename MapType::const_iterator&, const typename MapType::const_iterator&)>> s(
        [comparator](const typename MapType::const_iterator& lhs, const typename MapType::const_iterator& rhs) -> bool { return comparator(*lhs, *rhs); });

    for (auto it = map.begin(); it != map.end(); ++it) {
        s.insert(it);
    }

    for (auto& elt : s) {
        if (!func(*elt)) {
            break;
        }
    }
}

template <typename T, typename FieldType>
static std::function<bool(const T&, const T&)> getComparator(const FieldType field, const bool desc) {
    return [field, desc](const T& lhs, const T& rhs) { return desc ? (lhs.second.*field > rhs.second.*field) : (lhs.second.*field < rhs.second.*field); };
}

void APIHandler::TokenHoldersGet(api::TokenHoldersResult& _return, const general::Address& token, int64_t offset, int64_t limit, const TokenHoldersSortField order,
                                 const bool desc) {
    if (!validatePagination(_return, *this, offset, limit)) {
        return;
    }

    bool found = false;

    using HMap = decltype(Token::holders);
    using HT = HMap::value_type;

    std::function<bool(const HT&, const HT&)> comparator;

    switch (order) {
        case TH_Balance:
            comparator = [desc](const HT& lhs, const HT& rhs) { return desc ^ (stod(lhs.second.balance) < stod(rhs.second.balance)); };
            break;
        case TH_TransfersCount:
            comparator = getComparator<HT>(&Token::HolderInfo::transfersCount, desc);
            break;
    }

    const csdb::Address addr = BlockChain::getAddressFromKey(token);
    tm.applyToInternal([&token, &addr, &found, &offset, &limit, &_return, comparator](const TokensMap& tm, const HoldersMap&) {
        auto tIt = tm.find(addr);
        if (tIt != tm.end()) {
            found = true;
            _return.count = (uint32_t) tIt->second.realHoldersCount;

            applyToSortedMap(tIt->second.holders, comparator, [&offset, &limit, &_return, &token](const HMap::value_type& t) {
                if (TokensMaster::isZeroAmount(t.second.balance)) {
                    return true;
                }
                if (--offset >= 0) {
                    return true;
                }

                api::TokenHolder th;

                th.holder = fromByteArray(t.first.public_key());
                th.token = token;
                th.balance = t.second.balance;
                th.transfersCount = (uint32_t) t.second.transfersCount;

                _return.holders.push_back(th);

                if (--limit == 0) {
                    return false;
                }

                return true;
            });
        }
    });

    SetResponseStatus(_return.status, found ? APIRequestStatusType::SUCCESS : APIRequestStatusType::FAILURE);
}

void APIHandler::TokensListGet(api::TokensListResult& _return, int64_t offset, int64_t limit, const TokensListSortField order, const bool desc) {
    if (!validatePagination(_return, *this, offset, limit)) {
        return;
    }

    using VT = TokensMap::value_type;
    std::function<bool(const VT&, const VT&)> comparator;

    switch (order) {
        case TL_Code:
            comparator = getComparator<VT>(&Token::symbol, desc);
            break;
        case TL_Name:
            comparator = getComparator<VT>(&Token::name, desc);
            break;
        case TL_Address:
            comparator = [desc](const VT& lhs, const VT& rhs) { return desc ^ (lhs.first < rhs.first); };
            break;
        case TL_TotalSupply:
            comparator = [desc](const VT& lhs, const VT& rhs) { return desc ^ (stod(lhs.second.totalSupply) < stod(rhs.second.totalSupply)); };
            break;
        case TL_HoldersCount:
            comparator = getComparator<VT>(&Token::realHoldersCount, desc);
            break;
        case TL_TransfersCount:
            comparator = getComparator<VT>(&Token::transfersCount, desc);
            break;
        case TL_TransactionsCount:
            comparator = getComparator<VT>(&Token::transactionsCount, desc);
            break;
    };

    tm.applyToInternal([&offset, &limit, &_return, comparator](const TokensMap& tm, const HoldersMap&) {
        _return.count = (uint32_t) tm.size();

        applyToSortedMap(tm, comparator, [&offset, &limit, &_return](const TokensMap::value_type& t) {
            if (--offset >= 0) {
                return true;
            }

            api::TokenInfo tok;
            putTokenInfo(tok, fromByteArray(t.first.public_key()), t.second);

            _return.tokens.push_back(tok);

            if (--limit == 0) {
                return false;
            }

            return true;
        });
    });

    SetResponseStatus(_return.status, APIRequestStatusType::SUCCESS);
}

//////////Wallets
typedef std::list<std::pair<const cs::WalletsCache::WalletData::Address*, const cs::WalletsCache::WalletData*>> WCSortedList;
template <typename T>
void walletStep(const cs::WalletsCache::WalletData::Address* addr, const cs::WalletsCache::WalletData* wd, const uint64_t num,
                std::function<const T&(const cs::WalletsCache::WalletData&)> getter, std::function<bool(const T&, const T&)> comparator, WCSortedList& lst) {
    assert(num > 0);

    const T& val = getter(*wd);
    if (lst.size() < num || comparator(val, getter(*(lst.back().second)))) {
        // Guess why I can't use std::upper_bound in here
        // C++ is not as expressive as I've imagined it to be...
        auto it = lst.begin();
        while (it != lst.end() && !comparator(val, getter(*(it->second)))) {/* <-- this looks more like Lisp, doesn't it... */
            ++it;
        }

        lst.insert(it, std::make_pair(addr, wd));
        if (lst.size() > num) {
            lst.pop_back();
        }
    }
}

template <typename T>
void iterateOverWallets(std::function<const T&(const cs::WalletsCache::WalletData&)> getter, const uint64_t num, const bool desc, WCSortedList& lst, BlockChain& bc) {
    using Comparer = std::function<bool(const T&, const T&)>;
    Comparer comparator = desc ? Comparer(std::greater<T>()) : Comparer(std::less<T>());

    bc.iterateOverWallets([&lst, num, getter, comparator](const cs::WalletsCache::WalletData::Address& addr, const cs::WalletsCache::WalletData& wd) {
        if (!addr.empty() && wd.balance_ >= csdb::Amount(0)) {
            walletStep(&addr, &wd, num, getter, comparator, lst);
        }
        return true;
    });
}

void APIHandler::WalletsGet(WalletsGetResult& _return, int64_t _offset, int64_t _limit, int8_t _ordCol, bool _desc) {
    if (!validatePagination(_return, *this, _offset, _limit)) {
        return;
    }

    SetResponseStatus(_return.status, APIRequestStatusType::SUCCESS);

    WCSortedList lst;
    const uint64_t num = _offset + _limit;

    if (_ordCol == 0) {  // Balance
        iterateOverWallets<csdb::Amount>([](const cs::WalletsCache::WalletData& wd) -> const csdb::Amount& { return wd.balance_; }, num, _desc, lst, s_blockchain);
    }
#ifdef MONITOR_NODE
    else if (_ordCol == 1) {  // TimeReg
        iterateOverWallets<uint64_t>([](const cs::WalletsCache::WalletData& wd) -> const uint64_t& { return wd.createTime_; }, num, _desc, lst, s_blockchain);
    }
    else {  // Tx count
        iterateOverWallets<uint64_t>([](const cs::WalletsCache::WalletData& wd) -> const uint64_t& { return wd.transNum_; }, num, _desc, lst, s_blockchain);
    }
#endif

    if (lst.size() < (uint64_t)_offset) {
        return;
    }

    auto ptr = lst.begin();
    std::advance(ptr, _offset);

    for (; ptr != lst.end(); ++ptr) {
        api::WalletInfo wi;
        const cs::Bytes addr_b((*(ptr->first)).begin(), (*(ptr->first)).end());
        wi.address = fromByteArray(addr_b);
        wi.balance.integral = ptr->second->balance_.integral();
        wi.balance.fraction = ptr->second->balance_.fraction();
#ifdef MONITOR_NODE
        wi.transactionsNumber = ptr->second->transNum_;
        wi.firstTransactionTime = ptr->second->createTime_;
#endif

        _return.wallets.push_back(wi);
    }

    _return.count = (uint32_t) s_blockchain.getWalletsCountWithBalance();
}

void APIHandler::TrustedGet(TrustedGetResult& _return, int32_t _page) {
#ifdef MONITOR_NODE
    const static uint32_t PER_PAGE = 256;
    SetResponseStatus(_return.status, APIRequestStatusType::SUCCESS);
    _page = std::max(int32_t(0), _page);

    uint32_t offset = _page * PER_PAGE;
    uint32_t limit = PER_PAGE;
    uint32_t total = 0;

    s_blockchain.iterateOverWriters([&_return, &offset, &limit, &total](const cs::WalletsCache::WalletData::Address& addr, const cs::WalletsCache::TrustedData& wd) {
        if (addr.empty()) {
            return true;
        }
        if (offset == 0) {
            if (limit > 0) {
                api::TrustedInfo wi;
                // const ::csdb::internal::byte_array addr_b(addr.begin(), addr.end());
                const cs::Bytes addr_b(addr.begin(), addr.end());
                wi.address = fromByteArray(addr_b);

                wi.timesWriter = uint32_t(wd.times);
                wi.timesTrusted = uint32_t(wd.times_trusted);
                wi.feeCollected.integral = wd.totalFee.integral();
                wi.feeCollected.fraction = wd.totalFee.fraction();

                _return.writers.push_back(wi);
                --limit;
            }
        }
        else {
            --offset;
        }

        ++total;
        return true;
    });
    _return.pages = (total / PER_PAGE) + (int)(total % PER_PAGE != 0);
#else
    ++_page;
    SetResponseStatus(_return.status, APIRequestStatusType::SUCCESS);
#endif
}

////////new

void APIHandler::SyncStateGet(api::SyncStateResult& _return) {
    _return.lastBlock = s_blockchain.loadBlock(s_blockchain.getLastHash()).sequence();
    _return.currRound = cs::Conveyer::instance().currentRoundNumber();
    SetResponseStatus(_return.status, APIRequestStatusType::SUCCESS);
}

void apiexec::APIEXECHandler::GetSeed(apiexec::GetSeedResult& _return, const general::AccessID accessId) {
    const auto opt_sequence = executor_.getSequence(accessId);
    if (!opt_sequence.has_value()) {
        //_return.status.message = "AccessId isn't correct!";
        SetResponseStatus(_return.status, APIRequestStatusType::FAILURE);
        return;
    }
    const auto hash_seq = blockchain_.getHashBySequence(opt_sequence.value());
    const auto hash = ::csdb::priv::crypto::calc_hash(hash_seq.to_binary());
    std::copy(hash.begin(), hash.end(), std::inserter(_return.seed, _return.seed.end()));
    SetResponseStatus(_return.status, APIRequestStatusType::SUCCESS);
}

void apiexec::APIEXECHandler::SendTransaction(apiexec::SendTransactionResult& _return, const general::AccessID accessId, const api::Transaction& transaction) {
    csunused(_return);
    csunused(accessId);
    // const csdb::Address addr = BlockChain::getAddressFromKey(transaction.source);
    // BlockChain::WalletData wallData{};
    // BlockChain::WalletId wallId{};
    // if (!blockchain_.findWalletData(addr, wallData, wallId)) {
    //  SetResponseStatus(_return.status, APIRequestStatusType::NOT_FOUND);
    //  return;
    //}
    // const_cast<api::Transaction&>(transaction).id = wallData.trxTail_.empty() ? 0 : wallData.trxTail_.getLastTransactionId() + 1;
    executor_.addInnerSendTransaction(accessId, executor_.make_transaction(transaction));
}

void apiexec::APIEXECHandler::WalletIdGet(api::WalletIdGetResult& _return, const general::AccessID accessId, const general::Address& address) {
    csunused(accessId);
    const csdb::Address addr = BlockChain::getAddressFromKey(address);
    BlockChain::WalletData wallData{};
    BlockChain::WalletId wallId{};
    if (!blockchain_.findWalletData(addr, wallData, wallId)) {
        SetResponseStatus(_return.status, APIRequestStatusType::NOT_FOUND);
        return;
    }
    _return.walletId = wallId;
    SetResponseStatus(_return.status, APIRequestStatusType::SUCCESS);
}

void apiexec::APIEXECHandler::SmartContractGet(SmartContractGetResult& _return, const general::AccessID accessId, const general::Address& address) {
    const auto addr = BlockChain::getAddressFromKey(address);
    auto opt_transaction_id = executor_.getDeployTrxn(addr);
    if (!opt_transaction_id.has_value()) {
        SetResponseStatus(_return.status, APIRequestStatusType::FAILURE);
        return;
    }

    auto trxn = blockchain_.loadTransaction(opt_transaction_id.value());
    const auto sci = deserialize<api::SmartContractInvocation>(trxn.user_field(0).value<std::string>());
    _return.byteCodeObjects = sci.smartContractDeploy.byteCodeObjects;
    const auto opt_state = executor_.getAccessState(accessId, addr);
    if (!opt_state.has_value()) {
        SetResponseStatus(_return.status, APIRequestStatusType::FAILURE);
        return;
    }
    _return.contractState = opt_state.value();
    _return.stateCanModify = (solver_.isContractLocked(addr) && executor_.isLockSmart(address, accessId)) ? true : false;

    SetResponseStatus(_return.status, APIRequestStatusType::SUCCESS);
}

void apiexec::APIEXECHandler::WalletBalanceGet(api::WalletBalanceGetResult& _return, const general::Address& address) {
    const csdb::Address addr = BlockChain::getAddressFromKey(address);
    BlockChain::WalletData wallData{};
    BlockChain::WalletId wallId{};
    if (!blockchain_.findWalletData(addr, wallData, wallId)) {
        _return.balance.integral = 0;
        _return.balance.fraction = 0;
    }
    else {
        _return.balance.integral = wallData.balance_.integral();
        _return.balance.fraction = static_cast<decltype(_return.balance.fraction)>(wallData.balance_.fraction());
    }
    SetResponseStatus(_return.status, APIRequestStatusType::SUCCESS);
}

void apiexec::APIEXECHandler::PoolGet(PoolGetResult& _return, const int64_t sequence) {
    auto poolBin = blockchain_.loadBlock(sequence).to_binary();
    _return.pool.reserve(poolBin.size());
    std::copy(poolBin.begin(), poolBin.end(), std::back_inserter(_return.pool));
    SetResponseStatus(_return.status, APIRequestStatusType::SUCCESS);
}
