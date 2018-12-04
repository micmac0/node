#include <consensus.hpp>
#include <solvercore.hpp>

#pragma warning(push)
#pragma warning(disable : 4267 4244 4100 4245)
#include <csnode/node.hpp>
#pragma warning(pop)

#include <csdb/currency.hpp>
#include <lib/system/logger.hpp>
#include <csnode/fee.hpp>

#include <chrono>

namespace cs
{

  void SolverCore::setKeysPair(const cs::PublicKey& pub, const cs::PrivateKey& priv)
  {
    public_key = pub;
    private_key = priv;
  }

  void SolverCore::gotRound(cs::RoundNumber rNum)
  {
    // previous solver implementation calls to runConsensus method() here
    // perform similar actions, but only in proper state (TrustedStage1State for now)

    // clear data
    markUntrusted.fill(0);

    if(!pstate) {
      return;
    }
    if(stateCompleted(pstate->onSyncTransactions(*pcontext, rNum))) {
      handleTransitions(Event::Transactions);
    }
  }

  const cs::PublicKey& SolverCore::getWriterPublicKey() const
  {
    // Previous solver returns confidant key with index equal result of takeDecision() method.
    // As analogue, found writer's index in stage3 if exists, otherwise return empty object as prev. solver does
    auto ptr = find_stage3(pnode->getConfidantNumber());
    if(ptr != nullptr) {
      const auto& trusted = cs::Conveyer::instance().currentRoundTable().confidants;
      if(trusted.size() >= ptr->writer) {
        return *(trusted.cbegin() + ptr->writer);
      }
    }
    // TODO: redesign getting ref to persistent object
    static cs::PublicKey empty {};
    return empty;
  }

  void SolverCore::gotBigBang()
  {
    // in case of bigbang resend all info we have got
    // assume normal (ordinary) nodes does not store stages
    const auto own_num = pnode->getConfidantNumber();
    const auto pstage1 = find_stage1(own_num);
    if(pstage1 != nullptr) {
      cslog() << "SolverCore: resend stage-1 after BigBang";
      pnode->sendStageOne(*pstage1);
    }
    else {
      cslog() << "SolverCore: stage-1 not ready to re-send after BigBang";
    }
    const auto pstage2 = find_stage2(own_num);
    if(pstage2 != nullptr) {
      cslog() << "SolverCore: resend stage-2 after BigBang";
      pnode->sendStageTwo(*pstage2);
    }
    else {
      cslog() << "SolverCore: stage-2 not ready to re-send after BigBang";
    }
    const auto pstage3 = find_stage3(own_num);
    if(pstage3 != nullptr) {
      cslog() << "SolverCore: resend stage-3 after BigBang";
      pnode->sendStageThree(*pstage3);
    }
    else {
      cslog() << "SolverCore: stage-3 not ready yet to re-send after BigBang";
    }
  }

  void SolverCore::gotTransaction(const csdb::Transaction& trans)
  {
    if(!pstate) {
      return;
    }
    // produces too much output:
    csdebug() << "SolverCore: got transaction " << trans.innerID() << " from " << trans.source().to_string();
    if(stateCompleted(pstate->onTransaction(*pcontext, trans))) {
      handleTransitions(Event::Transactions);
    }
  }

  void SolverCore::gotBlock(csdb::Pool&& p, const cs::PublicKey& sender)
  {
    if(!pstate) {
      return;
    }
    csdebug() << "SolverCore: gotBlock()";
    if(stateCompleted(pstate->onBlock(*pcontext, p, sender))) {
      handleTransitions(Event::Block);
    }
  }

  void SolverCore::gotBlockRequest(const csdb::PoolHash& p_hash)
  {
    std::ostringstream os;
    os << "SolverCore: got request for block, ";
    // state does not take part
    if(pnode != nullptr) {
      csdb::Pool p = pnode->getBlockChain().loadBlock(p_hash);
      if(p.is_valid()) {
        os << "[" << p.sequence() << "] found, sending";
        //                pnode->sendBlockReply(p, sender);
      }
      else {
        os << "not found";
      }
    }
    else {
      os << "cannot handle";
    }
    if(Consensus::Log) {
      csinfo() << os.str();
    }
  }

  void SolverCore::gotBlockReply(csdb::Pool&)
  {
    if(!pstate) {
      return;
    }
    // "uncache" stored hashes if any
    if(!recv_hash.empty()) {
      if(cur_round - pnode->getBlockChain().getLastWrittenSequence() == 1) {
        for(const auto& hash_info : recv_hash) {
          if(stateCompleted(pstate->onHash(*pcontext, hash_info.first, hash_info.second))) {
            handleTransitions(Event::Hashes);
          }
        }
      }
    }
  }

  void SolverCore::gotHash(csdb::PoolHash&& hash, const cs::PublicKey& sender)
  {
    csdb::Pool::sequence_t delta = cur_round - pnode->getBlockChain().getLastWrittenSequence();
    if(delta > 1) {
      recv_hash.push_back(std::make_pair<>(hash, sender));
      csdebug() << "SolverCore: cache hash until last block ready";
      return;
    }

    if(!pstate) {
      return;
    }

    if(stateCompleted(pstate->onHash(*pcontext, hash, sender))) {
      handleTransitions(Event::Hashes);
    }
  }

  void SolverCore::beforeNextRound()
  {
    if(!pstate) {
      return;
    }
    pstate->onRoundEnd(*pcontext, false /*is_bigbang*/);
  }

  void SolverCore::nextRound()
  {
    if(pnode != nullptr) {
      auto tmp = pnode->getRoundNumber();
      if(cur_round == tmp) {
        cswarning() << "SolverCore: current round #" << tmp << " restarted (BigBang?)";
      }
      cur_round = tmp;
    }
    else {
      cur_round = 1;
    }

    // as store result of current round:
    if(Consensus::Log) {
      LOG_DEBUG("SolverCore: clear all stored round data (block hashes, stages-1..3)");
    }

    recv_hash.clear();
    stageOneStorage.clear();
    stageTwoStorage.clear();
    stageThreeStorage.clear();
    trusted_candidates.clear();

    if(!pstate) {
      return;
    }

    // update desired count of trusted nodes
    size_t cnt_trusted = cs::Conveyer::instance().currentRoundTable().confidants.size();
    if(cnt_trusted > cnt_trusted_desired) {
      cnt_trusted_desired = cnt_trusted;
    }

    // start timeout tracking
    auto round = cur_round;
    track_next_round.start(
      scheduler,
      Consensus::PostConsensusTimeout,
      [this, round]() {
        if(this->cur_round == round) {
          // round have not been changed yet
          cswarning() << "SolverCore: request next round info due to timeout " << Consensus::PostConsensusTimeout / 1000 << " sec";
          pnode->sendNextRoundRequest();
        }
      },
      true /*replace exisiting*/);

    if(stateCompleted(pstate->onRoundTable(*pcontext, static_cast<uint32_t>(cur_round)))) {
      handleTransitions(Event::RoundTable);
    }
  }

  void SolverCore::gotStageOne(const cs::StageOne& stage)
  {
    if(find_stage1(stage.sender) != nullptr) {
      // duplicated
      return;
    }

    stageOneStorage.push_back(stage);
    LOG_NOTICE("SolverCore: <-- stage-1 [" << (int) stage.sender << "] = " << stageOneStorage.size());

    if(!pstate) {
      return;
    }
    if(stateCompleted(pstate->onStage1(*pcontext, stage))) {
      handleTransitions(Event::Stage1Enough);
    }
  }

  void SolverCore::gotStageOneRequest(uint8_t requester, uint8_t required)
  {
    LOG_NOTICE("SolverCore: [" << (int) requester << "] asks for stage-1 of [" << (int) required << "]");
    const auto ptr = find_stage1(required);
    if(ptr != nullptr) {
      pnode->sendStageOneReply(*ptr, requester);
    }
  }

  void SolverCore::gotStageTwoRequest(uint8_t requester, uint8_t required)
  {
    LOG_NOTICE("SolverCore: [" << (int) requester << "] asks for stage-2 of [" << (int) required << "]");
    const auto ptr = find_stage2(required);
    if(ptr != nullptr) {
      pnode->sendStageTwoReply(*ptr, requester);
    }
  }

  void SolverCore::gotStageThreeRequest(uint8_t requester, uint8_t required)
  {
    LOG_NOTICE("SolverCore: [" << (int) requester << "] asks for stage-3 of [" << (int) required << "]");
    const auto ptr = find_stage3(required);
    if(ptr != nullptr) {
      pnode->sendStageThreeReply(*ptr, requester);
    }
  }

  void SolverCore::gotStageTwo(const cs::StageTwo& stage)
  {
    if(find_stage2(stage.sender) != nullptr) {
      // duplicated
      return;
    }

    stageTwoStorage.push_back(stage);
    LOG_NOTICE("SolverCore: <-- stage-2 [" << (int) stage.sender << "] = " << stageTwoStorage.size());

    if(!pstate) {
      return;
    }
    if(stateCompleted(pstate->onStage2(*pcontext, stage))) {
      handleTransitions(Event::Stage2Enough);
    }
  }

  void SolverCore::gotStageThree(const cs::StageThree& stage)
  {
    if(find_stage3(stage.sender) != nullptr) {
      // duplicated
      return;
    }

    stageThreeStorage.push_back(stage);
    LOG_NOTICE("SolverCore: <-- stage-3 [" << (int) stage.sender << "] = " << stageThreeStorage.size());

    if(!pstate) {
      return;
    }
    if(stateCompleted(pstate->onStage3(*pcontext, stage))) {
      handleTransitions(Event::Stage3Enough);
    }
  }

  void SolverCore::send_wallet_transaction(const csdb::Transaction& tr)
  {
    cs::Conveyer::instance().addTransaction(tr);
  }

  void SolverCore::gotRoundInfoRequest(const cs::PublicKey& requester, cs::RoundNumber requester_round)
  {
    cslog() << "SolverCore: got request for round info from "
      << cs::Utils::byteStreamToHex(requester.data(), requester.size());

    if(requester_round == cur_round) {
      const auto ptr = /*cur_round == 10 ? nullptr :*/ find_stage3(pnode->getConfidantNumber());
      if(ptr != nullptr) {
        if(ptr->sender == ptr->writer) {
          if(pnode->tryResendRoundInfo(requester, (cs::RoundNumber)cur_round)) {
            cslog() << "SolverCore: re-send full round info #" << cur_round << " completed";
            return;
          }
        }
      }
      cslog() << "SolverCore: also on the same round, inform cannot help with";
      pnode->sendRoundInfoReply(requester, false);
    }
    else if(requester_round < cur_round) {
      for(const auto& node : pnode->confidants()) {
        if(requester == node) {
          if(pnode->tryResendRoundInfo(requester, (cs::RoundNumber)cur_round)) {
            cslog() << "SolverCore: requester is trusted next round, supply it with round info";
            return;
          }
          cslog() << "SolverCore: try but cannot send full round info";
          break;
        }
      }
      cslog() << "SolverCore: inform requester next round has come";
      pnode->sendRoundInfoReply(requester, true);
    }
    else {
      // requester_round > cur_round, cannot help with!
      cslog() << "SolverCore: cannot help with outrunning round info";
    }
  }

  void SolverCore::gotRoundInfoReply(bool next_round_started, const cs::PublicKey& /*respondent*/)
  {
    if(next_round_started) {
      cslog() << "SolverCore: round info reply means next round started, and I am not trusted node. Waiting next round";
      return;
    }
    cswarning() << "SolverCore: round info reply means next round is not started, become writer in 2 sec";
    size_t stored_round = cur_round;
    scheduler.InsertOnce(1000,
      [this, stored_round]() {
      if(stored_round == cur_round) {
        // still did not receive next round info - become writer
        cserror() << "SolverCore: re-assign writer node is not completely implemented yet, cancel";
        //handleTransitions(SolverCore::Event::SetWriter);
      }
    },
      true);
  }

}  // namespace slv2
