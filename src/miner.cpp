// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin Core developers
// Copyright (c) 2016-2022 The Zcash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#include "miner.h"
#ifdef ENABLE_MINING
#include "pow/tromp/equi_miner.h"
#endif

#include "amount.h"
#include "chainparams.h"
#include "consensus/consensus.h"
#include "consensus/funding.h"
#include "consensus/upgrades.h"
#include "consensus/validation.h"
#ifdef ENABLE_MINING
#include "crypto/equihash.h"
#endif
#include "hash.h"
#include "key_io.h"
#include "main.h"
#include "metrics.h"
#include "net.h"
#include "zcash/Note.hpp"
#include "policy/policy.h"
#include "pow.h"
#include "primitives/transaction.h"
#include "random.h"
#include "timedata.h"
#include "transaction_builder.h"
#include "ui_interface.h"
#include "util/system.h"
#include "util/moneystr.h"
#include "validationinterface.h"

#include <librustzcash.h>

#include <boost/thread.hpp>
#include <boost/tuple/tuple.hpp>
#ifdef ENABLE_MINING
#include <functional>
#endif
#include <mutex>

using namespace std;

//////////////////////////////////////////////////////////////////////////////
//
// BitcoinMiner
//

//
// Unconfirmed transactions in the memory pool often depend on other
// transactions in the memory pool. When we select transactions from the
// pool, we select by highest priority or fee rate, so we might consider
// transactions that depend on transactions that aren't yet in the block.
// The COrphan class keeps track of these 'temporary orphans' while
// CreateBlock is figuring out which transactions to include.
//
class COrphan
{
public:
    const CTransaction* ptx;
    set<uint256> setDependsOn;
    CFeeRate feeRate;
    CAmount feePaid;
    double dPriority;

    COrphan(const CTransaction* ptxIn) : ptx(ptxIn), feeRate(0), feePaid(0), dPriority(0)
    {
    }
};

std::optional<uint64_t> last_block_num_txs;
std::optional<uint64_t> last_block_size;

// We want to sort transactions by priority and fee rate, so:
typedef boost::tuple<double, CFeeRate, CAmount, const CTransaction*> TxPriority;
class TxPriorityCompare
{
    bool byFee;

public:
    TxPriorityCompare(bool _byFee) : byFee(_byFee) { }

    bool operator()(const TxPriority& a, const TxPriority& b)
    {
        if (byFee)
        {
            if (a.get<1>() == b.get<1>())
                return a.get<0>() < b.get<0>();
            return a.get<1>() < b.get<1>();
        }
        else
        {
            if (a.get<0>() == b.get<0>())
                return a.get<1>() < b.get<1>();
            return a.get<0>() < b.get<0>();
        }
    }
};

void UpdateTime(CBlockHeader* pblock, const Consensus::Params& consensusParams, const CBlockIndex* pindexPrev)
{
    auto medianTimePast = pindexPrev->GetMedianTimePast();
    auto nTime = std::max(medianTimePast + 1, GetTime());
    // See the comment in ContextualCheckBlockHeader() for background.
    if (consensusParams.FutureTimestampSoftForkActive(pindexPrev->nHeight + 1)) {
        nTime = std::min(nTime, medianTimePast + MAX_FUTURE_BLOCK_TIME_MTP);
    }
    pblock->nTime = nTime;

    // Updating time can change work required on testnet:
    if (consensusParams.nPowAllowMinDifficultyBlocksAfterHeight != std::nullopt) {
        pblock->nBits = GetNextWorkRequired(pindexPrev, pblock, consensusParams);
    }
}

bool IsShieldedMinerAddress(const MinerAddress& minerAddr) {
    return !std::holds_alternative<boost::shared_ptr<CReserveScript>>(minerAddr);
}

class AddFundingStreamValueToTx
{
private:
    CMutableTransaction &mtx;
    void* ctx;
    const CAmount fundingStreamValue;
    const libzcash::Zip212Enabled zip212Enabled;
public:
    AddFundingStreamValueToTx(
            CMutableTransaction &mtx,
            void* ctx,
            const CAmount fundingStreamValue,
            const libzcash::Zip212Enabled zip212Enabled): mtx(mtx), ctx(ctx), fundingStreamValue(fundingStreamValue), zip212Enabled(zip212Enabled) {}

    bool operator()(const libzcash::SaplingPaymentAddress& pa) const {
        uint256 ovk;
        auto note = libzcash::SaplingNote(pa, fundingStreamValue, zip212Enabled);
        auto output = OutputDescriptionInfo(ovk, note, NO_MEMO);

        auto odesc = output.Build(ctx);
        if (odesc) {
            mtx.vShieldedOutput.push_back(odesc.value());
            mtx.valueBalanceSapling -= fundingStreamValue;
            return true;
        } else {
            return false;
        }
    }

    bool operator()(const CScript& scriptPubKey) const {
        mtx.vout.push_back(CTxOut(fundingStreamValue, scriptPubKey));
        return true;
    }
};


class AddOutputsToCoinbaseTxAndSign
{
private:
    CMutableTransaction &mtx;
    const CChainParams &chainparams;
    const int nHeight;
    const CAmount nFees;

public:
    AddOutputsToCoinbaseTxAndSign(
        CMutableTransaction &mtx,
        const CChainParams &chainparams,
        const int nHeight,
        const CAmount nFees) : mtx(mtx), chainparams(chainparams), nHeight(nHeight), nFees(nFees) {}

    const libzcash::Zip212Enabled GetZip212Flag() const {
        if (chainparams.GetConsensus().NetworkUpgradeActive(nHeight, Consensus::UPGRADE_CANOPY)) {
            return libzcash::Zip212Enabled::AfterZip212;
        } else {
            return libzcash::Zip212Enabled::BeforeZip212;
        }
    }

    CAmount SetFoundersRewardAndGetMinerValue(void* ctx) const {
        auto block_subsidy = GetBlockSubsidy(nHeight, chainparams.GetConsensus());
        auto miner_reward = block_subsidy; // founders' reward or funding stream amounts will be subtracted below

        if (nHeight > 0) {
            if (chainparams.GetConsensus().NetworkUpgradeActive(nHeight, Consensus::UPGRADE_CANOPY)) {
                auto fundingStreamElements = Consensus::GetActiveFundingStreamElements(
                    nHeight,
                    block_subsidy,
                    chainparams.GetConsensus());

                for (Consensus::FundingStreamElement fselem : fundingStreamElements) {
                    miner_reward -= fselem.second;
                    bool added = std::visit(AddFundingStreamValueToTx(mtx, ctx, fselem.second, GetZip212Flag()), fselem.first);
                    if (!added) {
                        librustzcash_sapling_proving_ctx_free(ctx);
                        throw new std::runtime_error("Failed to add funding stream output.");
                    }
                }
            } else if (nHeight <= chainparams.GetConsensus().GetLastFoundersRewardBlockHeight(nHeight)) {
                // Founders reward is 20% of the block subsidy
                auto vFoundersReward = miner_reward / 5;
                // Take some reward away from us
                miner_reward -= vFoundersReward;
                // And give it to the founders
                mtx.vout.push_back(CTxOut(vFoundersReward, chainparams.GetFoundersRewardScriptAtHeight(nHeight)));
            } else {
                // Founders reward ends without replacement if Canopy is not activated by the
                // last Founders' Reward block height + 1.
            }
        }

        return miner_reward + nFees;
    }

    void ComputeBindingSig(void* saplingCtx, std::optional<orchard::UnauthorizedBundle> orchardBundle) const {
        // Empty output script.
        uint256 dataToBeSigned;
        try {
            if (orchardBundle.has_value()) {
                // Orchard is only usable with v5+ transactions.
                dataToBeSigned = ProduceZip244SignatureHash(mtx, {}, orchardBundle.value());
            } else {
                CScript scriptCode;
                PrecomputedTransactionData txdata(mtx, {});
                dataToBeSigned = SignatureHash(
                    scriptCode, mtx, NOT_AN_INPUT, SIGHASH_ALL, 0,
                    CurrentEpochBranchId(nHeight, chainparams.GetConsensus()),
                    txdata);
            }
        } catch (std::logic_error ex) {
            librustzcash_sapling_proving_ctx_free(saplingCtx);
            throw ex;
        }

        if (orchardBundle.has_value()) {
            auto authorizedBundle = orchardBundle.value().ProveAndSign({}, dataToBeSigned);
            if (authorizedBundle.has_value()) {
                mtx.orchardBundle = authorizedBundle.value();
            } else {
                librustzcash_sapling_proving_ctx_free(saplingCtx);
                throw new std::runtime_error("Failed to create Orchard proof or signatures");
            }
        }

        bool success = librustzcash_sapling_binding_sig(
            saplingCtx,
            mtx.valueBalanceSapling,
            dataToBeSigned.begin(),
            mtx.bindingSig.data());

        if (!success) {
            librustzcash_sapling_proving_ctx_free(saplingCtx);
            throw new std::runtime_error("An error occurred computing the binding signature.");
        }
    }

    // Create Orchard output
    void operator()(const libzcash::OrchardRawAddress &to) const {
        auto ctx = librustzcash_sapling_proving_ctx_init();

        // `enableSpends` must be set to `false` for coinbase transactions. This
        // means the Orchard anchor is unconstrained, so we set it to the empty
        // tree root via a null (all zeroes) uint256.
        uint256 orchardAnchor;
        auto builder = orchard::Builder(false, true, orchardAnchor);

        // Shielded coinbase outputs must be recoverable with an all-zeroes ovk.
        uint256 ovk;
        auto miner_reward = SetFoundersRewardAndGetMinerValue(ctx);
        builder.AddOutput(ovk, to, miner_reward, std::nullopt);

        // orchard::Builder pads to two Actions, but does so using a "no OVK" policy for
        // dummy outputs, which violates coinbase rules requiring all shielded outputs to
        // be recoverable. We manually add a dummy output to sidestep this issue.
        // TODO: If/when we have funding streams going to Orchard recipients, this dummy
        // output can be removed.
        RawHDSeed rawSeed(32, 0);
        GetRandBytes(rawSeed.data(), 32);
        auto dummyTo = libzcash::OrchardSpendingKey::ForAccount(HDSeed(rawSeed), Params().BIP44CoinType(), 0)
            .ToFullViewingKey()
            .ToIncomingViewingKey()
            .Address(0);
        builder.AddOutput(ovk, dummyTo, 0, std::nullopt);

        auto bundle = builder.Build();
        if (!bundle.has_value()) {
            librustzcash_sapling_proving_ctx_free(ctx);
            throw new std::runtime_error("Failed to create shielded output for miner");
        }

        ComputeBindingSig(ctx, std::move(bundle));

        librustzcash_sapling_proving_ctx_free(ctx);
    }

    // Create shielded output
    void operator()(const libzcash::SaplingPaymentAddress &pa) const {
        auto ctx = librustzcash_sapling_proving_ctx_init();

        auto miner_reward = SetFoundersRewardAndGetMinerValue(ctx);
        mtx.valueBalanceSapling -= miner_reward;

        uint256 ovk;

        auto note = libzcash::SaplingNote(pa, miner_reward, GetZip212Flag());
        auto output = OutputDescriptionInfo(ovk, note, NO_MEMO);

        auto odesc = output.Build(ctx);
        if (!odesc) {
            librustzcash_sapling_proving_ctx_free(ctx);
            throw new std::runtime_error("Failed to create shielded output for miner");
        }
        mtx.vShieldedOutput.push_back(odesc.value());

        ComputeBindingSig(ctx, std::nullopt);

        librustzcash_sapling_proving_ctx_free(ctx);
    }

    // Create transparent output
    void operator()(const boost::shared_ptr<CReserveScript> &coinbaseScript) const {
        // Add the FR output and fetch the miner's output value.
        auto ctx = librustzcash_sapling_proving_ctx_init();

        // Miner output will be vout[0]; Founders' Reward & funding stream outputs
        // will follow.
        mtx.vout.resize(1);
        auto value = SetFoundersRewardAndGetMinerValue(ctx);

        // Now fill in the miner's output.
        mtx.vout[0] = CTxOut(value, coinbaseScript->reserveScript);

        if (mtx.vShieldedOutput.size() > 0) {
            ComputeBindingSig(ctx, std::nullopt);
        }

        librustzcash_sapling_proving_ctx_free(ctx);
    }
};

CMutableTransaction CreateCoinbaseTransaction(const CChainParams& chainparams, CAmount nFees, const MinerAddress& minerAddress, int nHeight)
{
        CMutableTransaction mtx = CreateNewContextualCMutableTransaction(
                chainparams.GetConsensus(), nHeight,
                !std::holds_alternative<libzcash::OrchardRawAddress>(minerAddress) && nPreferredTxVersion < ZIP225_MIN_TX_VERSION);
        mtx.vin.resize(1);
        mtx.vin[0].prevout.SetNull();
        if (chainparams.GetConsensus().NetworkUpgradeActive(nHeight, Consensus::UPGRADE_NU5)) {
            // ZIP 203: From NU5 onwards, nExpiryHeight is set to the block height in
            // coinbase transactions.
            mtx.nExpiryHeight = nHeight;
        } else {
            // Set to 0 so expiry height does not apply to coinbase txs
            mtx.nExpiryHeight = 0;
        }

        // Add outputs and sign
        std::visit(
            AddOutputsToCoinbaseTxAndSign(mtx, chainparams, nHeight, nFees),
            minerAddress);

        mtx.vin[0].scriptSig = CScript() << nHeight << OP_0;
        return mtx;
}

CBlockTemplate* CreateNewBlock(const CChainParams& chainparams, const MinerAddress& minerAddress, const std::optional<CMutableTransaction>& next_cb_mtx)
{
    // Create new block
    std::unique_ptr<CBlockTemplate> pblocktemplate(new CBlockTemplate());
    if(!pblocktemplate.get())
        return NULL;
    CBlock *pblock = &pblocktemplate->block; // pointer for convenience

    // -regtest only: allow overriding block.nVersion with
    // -blockversion=N to test forking scenarios
    if (chainparams.MineBlocksOnDemand())
        pblock->nVersion = GetArg("-blockversion", pblock->nVersion);

    // Add dummy coinbase tx as first transaction
    pblock->vtx.push_back(CTransaction());
    pblocktemplate->vTxFees.push_back(-1); // updated at end
    pblocktemplate->vTxSigOps.push_back(-1); // updated at end

    // Largest block you're willing to create:
    unsigned int nBlockMaxSize = GetArg("-blockmaxsize", DEFAULT_BLOCK_MAX_SIZE);
    // Limit to between 1K and MAX_BLOCK_SIZE-1K for sanity:
    nBlockMaxSize = std::max((unsigned int)1000, std::min((unsigned int)(MAX_BLOCK_SIZE-1000), nBlockMaxSize));

    // How much of the block should be dedicated to high-priority transactions,
    // included regardless of the fees they pay
    unsigned int nBlockPrioritySize = GetArg("-blockprioritysize", DEFAULT_BLOCK_PRIORITY_SIZE);
    nBlockPrioritySize = std::min(nBlockMaxSize, nBlockPrioritySize);

    // Minimum block size you want to create; block will be filled with free transactions
    // until there are no more or the block reaches this size:
    unsigned int nBlockMinSize = GetArg("-blockminsize", DEFAULT_BLOCK_MIN_SIZE);
    nBlockMinSize = std::min(nBlockMaxSize, nBlockMinSize);

    // Collect memory pool transactions into the block
    CAmount nFees = 0;

    {
        LOCK2(cs_main, mempool.cs);
        CBlockIndex* pindexPrev = chainActive.Tip();
        const int nHeight = pindexPrev->nHeight + 1;
        uint32_t consensusBranchId = CurrentEpochBranchId(nHeight, chainparams.GetConsensus());
        pblock->nTime = GetTime();
        const int64_t nMedianTimePast = pindexPrev->GetMedianTimePast();
        CCoinsViewCache view(pcoinsTip);

        SaplingMerkleTree sapling_tree;
        assert(view.GetSaplingAnchorAt(view.GetBestAnchor(SAPLING), sapling_tree));

        // Priority order to process transactions
        list<COrphan> vOrphan; // list memory doesn't move
        map<uint256, vector<COrphan*> > mapDependers;
        bool fPrintPriority = GetBoolArg("-printpriority", DEFAULT_PRINTPRIORITY);

        // This vector will be sorted into a priority queue:
        vector<TxPriority> vecPriority;
        vecPriority.reserve(mempool.mapTx.size());

        // If we're given a coinbase tx, it's been precomputed, its fees are zero,
        // so we can't include any mempool transactions; this will be an empty block.
        if (!next_cb_mtx) {
            for (CTxMemPool::indexed_transaction_set::iterator mi = mempool.mapTx.begin();
                mi != mempool.mapTx.end(); ++mi)
            {
                const CTransaction& tx = mi->GetTx();

                int64_t nLockTimeCutoff = (STANDARD_LOCKTIME_VERIFY_FLAGS & LOCKTIME_MEDIAN_TIME_PAST)
                                        ? nMedianTimePast
                                        : pblock->GetBlockTime();

                if (tx.IsCoinBase() || !IsFinalTx(tx, nHeight, nLockTimeCutoff) || IsExpiredTx(tx, nHeight))
                    continue;

                COrphan* porphan = NULL;
                double dPriority = 0;
                CAmount nTotalIn = 0;
                bool fMissingInputs = false;
                for (const CTxIn& txin : tx.vin)
                {
                    // Read prev transaction
                    if (!view.HaveCoins(txin.prevout.hash))
                    {
                        LogPrintf("INFO: missing coins for %s", txin.prevout.hash.GetHex());
                        // This should never happen; all transactions in the memory
                        // pool should connect to either transactions in the chain
                        // or other transactions in the memory pool.
                        if (!mempool.mapTx.count(txin.prevout.hash))
                        {
                            LogPrintf("ERROR: mempool transaction missing input\n");
                            if (fDebug) assert("mempool transaction missing input" == 0);
                            fMissingInputs = true;
                            if (porphan)
                                vOrphan.pop_back();
                            break;
                        }

                        // Has to wait for dependencies
                        if (!porphan)
                        {
                            // Use list for automatic deletion
                            vOrphan.push_back(COrphan(&tx));
                            porphan = &vOrphan.back();
                        }
                        mapDependers[txin.prevout.hash].push_back(porphan);
                        porphan->setDependsOn.insert(txin.prevout.hash);
                        nTotalIn += mempool.mapTx.find(txin.prevout.hash)->GetTx().vout[txin.prevout.n].nValue;
                        continue;
                    }
                    const CCoins* coins = view.AccessCoins(txin.prevout.hash);
                    assert(coins);

                    CAmount nValueIn = coins->vout[txin.prevout.n].nValue;
                    nTotalIn += nValueIn;

                    int nConf = nHeight - coins->nHeight;

                    dPriority += (double)nValueIn * nConf;
                }
                nTotalIn += tx.GetShieldedValueIn();

                if (fMissingInputs) continue;

                // Priority is sum(valuein * age) / modified_txsize
                unsigned int nTxSize = ::GetSerializeSize(tx, SER_NETWORK, PROTOCOL_VERSION);
                dPriority = tx.ComputePriority(dPriority, nTxSize);

                uint256 hash = tx.GetHash();
                mempool.ApplyDeltas(hash, dPriority, nTotalIn);

                CAmount feePaid = nTotalIn - tx.GetValueOut();
                CFeeRate feeRate(feePaid, nTxSize);

                if (porphan)
                {
                    porphan->dPriority = dPriority;
                    porphan->feeRate = feeRate;
                    porphan->feePaid = feePaid;
                }
                else
                    vecPriority.push_back(TxPriority(dPriority, feeRate, feePaid, &(mi->GetTx())));
            }
        }

        // Collect transactions into block
        uint64_t nBlockSize = 1000;
        uint64_t nBlockTx = 0;
        int nBlockSigOps = 100;
        bool fSortedByFee = (nBlockPrioritySize <= 0);

        TxPriorityCompare comparer(fSortedByFee);
        std::make_heap(vecPriority.begin(), vecPriority.end(), comparer);

        // We want to track the value pool, but if the miner gets
        // invoked on an old block before the hardcoded fallback
        // is active we don't want to trip up any assertions. So,
        // we only adhere to the turnstile (as a miner) if we
        // actually have all of the information necessary to do
        // so.
        CAmount sproutValue = 0;
        CAmount saplingValue = 0;
        CAmount orchardValue = 0;
        bool monitoring_pool_balances = true;
        if (chainparams.ZIP209Enabled()) {
            if (pindexPrev->nChainSproutValue) {
                sproutValue = *pindexPrev->nChainSproutValue;
            } else {
                monitoring_pool_balances = false;
            }
            if (pindexPrev->nChainSaplingValue) {
                saplingValue = *pindexPrev->nChainSaplingValue;
            } else {
                monitoring_pool_balances = false;
            }
            if (pindexPrev->nChainOrchardValue) {
                orchardValue = *pindexPrev->nChainOrchardValue;
            } else {
                monitoring_pool_balances = false;
            }
        }

        LogPrintf("%s: Evaluating %u transactions for inclusion in block.", __func__, vecPriority.size());
        while (!vecPriority.empty())
        {
            // Take highest priority transaction off the priority queue:
            double dPriority = vecPriority.front().get<0>();
            CFeeRate feeRate = vecPriority.front().get<1>();
            CAmount feePaid = vecPriority.front().get<2>();
            const CTransaction& tx = *(vecPriority.front().get<3>());
            const uint256& hash = tx.GetHash();

            std::pop_heap(vecPriority.begin(), vecPriority.end(), comparer);
            vecPriority.pop_back();

            // Size limits
            unsigned int nTxSize = ::GetSerializeSize(tx, SER_NETWORK, PROTOCOL_VERSION);
            if (nBlockSize + nTxSize >= nBlockMaxSize) {
                LogPrintf("%s: skipping tx %s: exceeded maximum block size %u.", __func__, hash.GetHex(), nBlockMaxSize);
                continue;
            }

            // Legacy limits on sigOps:
            unsigned int nTxSigOps = GetLegacySigOpCount(tx);
            if (nBlockSigOps + nTxSigOps >= MAX_BLOCK_SIGOPS) {
                LogPrintf("%s: skipping tx %s: exceeds legacy max sigops %u.", __func__, hash.GetHex(), MAX_BLOCK_SIGOPS);
                continue;
            }

            // Skip free transactions if we're past the minimum block size:
            double dPriorityDelta = 0;
            CAmount nFeeDelta = 0;
            mempool.ApplyDeltas(hash, dPriorityDelta, nFeeDelta);
            if (fSortedByFee &&
                (dPriorityDelta <= 0) &&
                (nFeeDelta <= 0) &&
                (feeRate < ::minRelayTxFee) &&
                (feePaid < DEFAULT_FEE) &&
                (nBlockSize + nTxSize >= nBlockMinSize))
            {
                LogPrintf(
                        "%s: skipping free tx %s (fee is %i; %s) with size %u, current block size is %u & already have minimum block size %u.",
                        __func__, hash.GetHex(),
                        feePaid, feeRate.ToString(), nTxSize, nBlockSize, nBlockMinSize);
                continue;
            }

            // Prioritise by fee once past the priority size or we run out of high-priority
            // transactions:
            if (!fSortedByFee &&
                ((nBlockSize + nTxSize >= nBlockPrioritySize) || !AllowFree(dPriority)))
            {
                fSortedByFee = true;
                comparer = TxPriorityCompare(fSortedByFee);
                std::make_heap(vecPriority.begin(), vecPriority.end(), comparer);
            }

            if (!view.HaveInputs(tx)) {
                LogPrintf("%s: not including tx %s; missing inputs.", __func__, hash.GetHex());
                continue;
            }

            CAmount nTxFees = view.GetValueIn(tx)-tx.GetValueOut();

            nTxSigOps += GetP2SHSigOpCount(tx, view);
            if (nBlockSigOps + nTxSigOps >= MAX_BLOCK_SIGOPS) {
                LogPrintf("%s: skipping tx %s: exceeds p2sh max sigops %u.", __func__, hash.GetHex(), MAX_BLOCK_SIGOPS);
                continue;
            }

            std::vector<CTxOut> allPrevOutputs;
            for (const auto& input : tx.vin) {
                allPrevOutputs.push_back(view.GetOutputFor(input));
            }

            // Note that flags: we don't want to set mempool/IsStandard()
            // policy here, but we still have to ensure that the block we
            // create only contains transactions that are valid in new blocks.
            CValidationState state;
            PrecomputedTransactionData txdata(tx, allPrevOutputs);
            if (!ContextualCheckInputs(tx, state, view, true, MANDATORY_SCRIPT_VERIFY_FLAGS, true, txdata, chainparams.GetConsensus(), consensusBranchId)) {
                LogPrintf("%s: skipping tx %s: Failed contextual inputs check.", __func__, hash.GetHex());
                continue;
            }

            if (chainparams.ZIP209Enabled() && monitoring_pool_balances) {
                // Does this transaction lead to a turnstile violation?

                CAmount sproutValueDummy = sproutValue;
                CAmount saplingValueDummy = saplingValue;
                CAmount orchardValueDummy = orchardValue;

                saplingValueDummy += -tx.GetValueBalanceSapling();
                orchardValueDummy += -tx.GetOrchardBundle().GetValueBalance();

                for (auto js : tx.vJoinSplit) {
                    sproutValueDummy += js.vpub_old;
                    sproutValueDummy -= js.vpub_new;
                }

                if (sproutValueDummy < 0) {
                    LogPrintf("%s: tx %s appears to violate Sprout turnstile\n", __func__, hash.GetHex());
                    continue;
                }
                if (saplingValueDummy < 0) {
                    LogPrintf("%s: tx %s appears to violate Sapling turnstile\n", __func__, hash.GetHex());
                    continue;
                }
                if (orchardValueDummy < 0) {
                    LogPrintf("%s: tx %s appears to violate Orchard turnstile\n", __func__, hash.GetHex());
                    continue;
                }

                sproutValue = sproutValueDummy;
                saplingValue = saplingValueDummy;
                orchardValue = orchardValueDummy;
            }

            UpdateCoins(tx, view, nHeight);

            // Added
            pblock->vtx.push_back(tx);
            pblocktemplate->vTxFees.push_back(nTxFees);
            pblocktemplate->vTxSigOps.push_back(nTxSigOps);
            nBlockSize += nTxSize;
            ++nBlockTx;
            nBlockSigOps += nTxSigOps;
            nFees += nTxFees;

            if (fPrintPriority)
            {
                LogPrintf("%s: priority %.1f fee %s txid %s\n",
                    __func__, dPriority, feeRate.ToString(), hash.GetHex());
            }

            // Add transactions that depend on this one to the priority queue
            if (mapDependers.count(hash))
            {
                for (COrphan* porphan : mapDependers[hash])
                {
                    if (!porphan->setDependsOn.empty())
                    {
                        porphan->setDependsOn.erase(hash);
                        if (porphan->setDependsOn.empty())
                        {
                            vecPriority.push_back(TxPriority(porphan->dPriority, porphan->feeRate, porphan->feePaid, porphan->ptx));
                            std::push_heap(vecPriority.begin(), vecPriority.end(), comparer);
                        }
                    }
                }
            }
        }

        last_block_num_txs = nBlockTx;
        last_block_size = nBlockSize;
        LogPrintf("%s: total tx: %u; total size: %u (excluding coinbase)", __func__, nBlockTx, nBlockSize);

        // Create coinbase tx
        if (next_cb_mtx) {
            pblock->vtx[0] = *next_cb_mtx;
        } else {
            pblock->vtx[0] = CreateCoinbaseTransaction(chainparams, nFees, minerAddress, nHeight);
        }
        pblocktemplate->vTxFees[0] = -nFees;

        // Update the Sapling commitment tree.
        for (const CTransaction& tx : pblock->vtx) {
            for (const OutputDescription& odesc : tx.vShieldedOutput) {
                sapling_tree.append(odesc.cmu);
            }
        }

        // Randomise nonce
        arith_uint256 nonce = UintToArith256(GetRandHash());
        // Clear the top and bottom 16 bits (for local use as thread flags and counters)
        nonce <<= 32;
        nonce >>= 16;
        pblock->nNonce = ArithToUint256(nonce);

        uint32_t prevConsensusBranchId = CurrentEpochBranchId(pindexPrev->nHeight, chainparams.GetConsensus());

        // Fill in header
        pblock->hashPrevBlock  = pindexPrev->GetBlockHash();
        if (chainparams.GetConsensus().NetworkUpgradeActive(nHeight, Consensus::UPGRADE_NU5)) {
            // hashBlockCommitments depends on the block transactions, so we have to
            // update it whenever the coinbase transaction changes.
            //
            // - For the internal miner (either directly or via the `generate` RPC), this
            //   will occur in `IncrementExtraNonce()`, like for `hashMerkleRoot`.
            // - For `getblocktemplate`, we have two sets of fields to handle:
            //   - The `defaultroots` fields, which contain both the default value (if
            //     nothing in the template is altered), and the roots that can be used to
            //     recalculate it (if some or all of the template is altered).
            //   - The legacy `finalsaplingroothash`, `lightclientroothash`, and
            //     `blockcommitmentshash` fields, which had the semantics of "place this
            //     value into the block header and things will work" (except for in
            //     v4.6.0 where they were accidentally set to always be the NU5 value).
            //
            // To accommodate all use cases, we calculate the `hashBlockCommitments`
            // default value here (unlike `hashMerkleRoot`), and additionally cache the
            // values necessary to recalculate it.
            pblocktemplate->hashChainHistoryRoot = view.GetHistoryRoot(prevConsensusBranchId);
            pblocktemplate->hashAuthDataRoot = pblock->BuildAuthDataMerkleTree();
            pblock->hashBlockCommitments = DeriveBlockCommitmentsHash(
                    pblocktemplate->hashChainHistoryRoot,
                    pblocktemplate->hashAuthDataRoot);
        } else if (IsActivationHeight(nHeight, chainparams.GetConsensus(), Consensus::UPGRADE_HEARTWOOD)) {
            pblocktemplate->hashChainHistoryRoot.SetNull();
            pblocktemplate->hashAuthDataRoot.SetNull();
            pblock->hashBlockCommitments.SetNull();
        } else if (chainparams.GetConsensus().NetworkUpgradeActive(nHeight, Consensus::UPGRADE_HEARTWOOD)) {
            pblocktemplate->hashChainHistoryRoot = view.GetHistoryRoot(prevConsensusBranchId);
            pblocktemplate->hashAuthDataRoot.SetNull();
            pblock->hashBlockCommitments = pblocktemplate->hashChainHistoryRoot;
        } else {
            pblocktemplate->hashChainHistoryRoot.SetNull();
            pblocktemplate->hashAuthDataRoot.SetNull();
            pblock->hashBlockCommitments = sapling_tree.root();
        }
        UpdateTime(pblock, chainparams.GetConsensus(), pindexPrev);
        pblock->nBits          = GetNextWorkRequired(pindexPrev, pblock, chainparams.GetConsensus());
        pblock->nSolution.clear();
        pblocktemplate->vTxSigOps[0] = GetLegacySigOpCount(pblock->vtx[0]);

        CValidationState state;
        if (!TestBlockValidity(state, chainparams, *pblock, pindexPrev, true))
            throw std::runtime_error(std::string("CreateNewBlock(): TestBlockValidity failed: ") + state.GetRejectReason());
    }

    return pblocktemplate.release();
}

//////////////////////////////////////////////////////////////////////////////
//
// Internal miner
//

#ifdef ENABLE_MINING

class MinerAddressScript : public CReserveScript
{
    // CReserveScript requires implementing this function, so that if an
    // internal (not-visible) wallet address is used, the wallet can mark it as
    // important when a block is mined (so it then appears to the user).
    // If -mineraddress is set, the user already knows about and is managing the
    // address, so we don't need to do anything here.
    void KeepScript() {}
};

std::optional<MinerAddress> ExtractMinerAddress::operator()(const CKeyID &keyID) const {
    boost::shared_ptr<MinerAddressScript> mAddr(new MinerAddressScript());
    mAddr->reserveScript = CScript() << OP_DUP << OP_HASH160 << ToByteVector(keyID) << OP_EQUALVERIFY << OP_CHECKSIG;
    return mAddr;
}
std::optional<MinerAddress> ExtractMinerAddress::operator()(const CScriptID &addr) const {
    return std::nullopt;
}
std::optional<MinerAddress> ExtractMinerAddress::operator()(const libzcash::SproutPaymentAddress &addr) const {
    return std::nullopt;
}
std::optional<MinerAddress> ExtractMinerAddress::operator()(const libzcash::SaplingPaymentAddress &addr) const {
    return addr;
}
std::optional<MinerAddress> ExtractMinerAddress::operator()(const libzcash::UnifiedAddress &addr) const {
    auto preferred = addr.GetPreferredRecipientAddress(consensus, height);
    if (preferred.has_value()) {
        std::optional<MinerAddress> ret;
        std::visit(match {
            [&](const libzcash::OrchardRawAddress addr) { ret = MinerAddress(addr); },
            [&](const libzcash::SaplingPaymentAddress addr) { ret = MinerAddress(addr); },
            [&](const CKeyID keyID) { ret = operator()(keyID); },
            [&](const auto other) { ret = std::nullopt; }
        }, preferred.value());
        return ret;
    } else {
        return std::nullopt;
    }
}


void GetMinerAddress(std::optional<MinerAddress> &minerAddress)
{
    KeyIO keyIO(Params());

    // If the user sets a UA miner address with an Orchard component, we want to ensure we
    // start using it once we reach that height.
    int height;
    {
        LOCK(cs_main);
        height = chainActive.Height() + 1;
    }

    auto mAddrArg = GetArg("-mineraddress", "");
    auto zaddr0 = keyIO.DecodePaymentAddress(mAddrArg);
    if (zaddr0.has_value()) {
        auto zaddr = std::visit(ExtractMinerAddress(Params().GetConsensus(), height), zaddr0.value());
        if (zaddr.has_value()) {
            minerAddress = zaddr.value();
        }
    }
}

void IncrementExtraNonce(
    CBlockTemplate* pblocktemplate,
    const CBlockIndex* pindexPrev,
    unsigned int& nExtraNonce,
    const Consensus::Params& consensusParams)
{
    CBlock *pblock = &pblocktemplate->block;
    // Update nExtraNonce
    static uint256 hashPrevBlock;
    if (hashPrevBlock != pblock->hashPrevBlock)
    {
        nExtraNonce = 0;
        hashPrevBlock = pblock->hashPrevBlock;
    }
    ++nExtraNonce;
    unsigned int nHeight = pindexPrev->nHeight+1; // Height first in coinbase required for block.version=2
    CMutableTransaction txCoinbase(pblock->vtx[0]);
    txCoinbase.vin[0].scriptSig = (CScript() << nHeight << CScriptNum(nExtraNonce)) + COINBASE_FLAGS;
    assert(txCoinbase.vin[0].scriptSig.size() <= 100);

    pblock->vtx[0] = txCoinbase;
    pblock->hashMerkleRoot = pblock->BuildMerkleTree();
    if (consensusParams.NetworkUpgradeActive(nHeight, Consensus::UPGRADE_NU5)) {
        pblocktemplate->hashAuthDataRoot = pblock->BuildAuthDataMerkleTree();
        pblock->hashBlockCommitments = DeriveBlockCommitmentsHash(
            pblocktemplate->hashChainHistoryRoot,
            pblocktemplate->hashAuthDataRoot);
    }
}

static bool ProcessBlockFound(const CBlock* pblock, const CChainParams& chainparams)
{
    LogPrintf("%s\n", pblock->ToString());
    LogPrintf("generated %s\n", FormatMoney(pblock->vtx[0].vout[0].nValue));

    // Found a solution
    {
        LOCK(cs_main);
        if (pblock->hashPrevBlock != chainActive.Tip()->GetBlockHash())
            return error("ZcashMiner: generated block is stale");
    }

    // Inform about the new block
    GetMainSignals().BlockFound(pblock->GetHash());

    // Process this block the same as if we had received it from another node
    CValidationState state;
    if (!ProcessNewBlock(state, chainparams, NULL, pblock, true, NULL))
        return error("ZcashMiner: ProcessNewBlock, block not accepted");

    TrackMinedBlock(pblock->GetHash());

    return true;
}

void static BitcoinMiner(const CChainParams& chainparams)
{
    LogPrintf("ZcashMiner started\n");
    SetThreadPriority(THREAD_PRIORITY_LOWEST);
    RenameThread("zcash-miner");

    // Each thread has its own counter
    unsigned int nExtraNonce = 0;

    std::optional<MinerAddress> maybeMinerAddress;
    GetMainSignals().AddressForMining(maybeMinerAddress);

    unsigned int n = chainparams.GetConsensus().nEquihashN;
    unsigned int k = chainparams.GetConsensus().nEquihashK;

    std::string solver = GetArg("-equihashsolver", "default");
    assert(solver == "tromp" || solver == "default");
    LogPrint("pow", "Using Equihash solver \"%s\" with n = %u, k = %u\n", solver, n, k);

    std::mutex m_cs;
    bool cancelSolver = false;
    boost::signals2::connection c = uiInterface.NotifyBlockTip.connect(
        [&m_cs, &cancelSolver](bool, const CBlockIndex *) mutable {
            std::lock_guard<std::mutex> lock{m_cs};
            cancelSolver = true;
        }
    );
    miningTimer.start();

    try {
        // Throw an error if no address valid for mining was provided.
        if (!(maybeMinerAddress.has_value() && std::visit(IsValidMinerAddress(), maybeMinerAddress.value()))) {
            throw std::runtime_error("No miner address available (mining requires a wallet or -mineraddress)");
        }
        auto minerAddress = maybeMinerAddress.value();

        while (true) {
            if (chainparams.MiningRequiresPeers()) {
                // Busy-wait for the network to come online so we don't waste time mining
                // on an obsolete chain. In regtest mode we expect to fly solo.
                miningTimer.stop();
                do {
                    bool fvNodesEmpty;
                    {
                        LOCK(cs_vNodes);
                        fvNodesEmpty = vNodes.empty();
                    }
                    if (!fvNodesEmpty && !IsInitialBlockDownload(chainparams.GetConsensus()))
                        break;
                    MilliSleep(1000);
                } while (true);
                miningTimer.start();
            }

            //
            // Create new block
            //
            unsigned int nTransactionsUpdatedLast = mempool.GetTransactionsUpdated();
            CBlockIndex* pindexPrev;
            {
                LOCK(cs_main);
                pindexPrev = chainActive.Tip();
            }

            // If we don't have a valid chain tip to work from, wait and try again.
            if (pindexPrev == nullptr) {
                MilliSleep(1000);
                continue;
            }

            unique_ptr<CBlockTemplate> pblocktemplate(CreateNewBlock(chainparams, minerAddress));
            if (!pblocktemplate.get())
            {
                if (GetArg("-mineraddress", "").empty()) {
                    LogPrintf("Error in ZcashMiner: Keypool ran out, please call keypoolrefill before restarting the mining thread\n");
                } else {
                    // Should never reach here, because -mineraddress validity is checked in init.cpp
                    LogPrintf("Error in ZcashMiner: Invalid -mineraddress\n");
                }
                return;
            }
            CBlock *pblock = &pblocktemplate->block;
            IncrementExtraNonce(pblocktemplate.get(), pindexPrev, nExtraNonce, chainparams.GetConsensus());

            LogPrintf("Running ZcashMiner with %u transactions in block (%u bytes)\n", pblock->vtx.size(),
                ::GetSerializeSize(*pblock, SER_NETWORK, PROTOCOL_VERSION));

            //
            // Search
            //
            int64_t nStart = GetTime();
            arith_uint256 hashTarget = arith_uint256().SetCompact(pblock->nBits);

            while (true) {
                // Hash state
                eh_HashState state = EhInitialiseState(n, k);

                // I = the block header minus nonce and solution.
                CEquihashInput I{*pblock};
                CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
                ss << I;

                // H(I||...
                state.Update((unsigned char*)&ss[0], ss.size());

                // H(I||V||...
                eh_HashState curr_state = state;
                curr_state.Update(pblock->nNonce.begin(), pblock->nNonce.size());

                // (x_1, x_2, ...) = A(I, V, n, k)
                LogPrint("pow", "Running Equihash solver \"%s\" with nNonce = %s\n",
                         solver, pblock->nNonce.ToString());

                std::function<bool(std::vector<unsigned char>)> validBlock =
                        [&pblock, &hashTarget, &chainparams, &m_cs, &cancelSolver, &minerAddress]
                        (std::vector<unsigned char> soln) {
                    // Write the solution to the hash and compute the result.
                    LogPrint("pow", "- Checking solution against target\n");
                    pblock->nSolution = soln;
                    solutionTargetChecks.increment();

                    if (UintToArith256(pblock->GetHash()) > hashTarget) {
                        return false;
                    }

                    // Found a solution
                    SetThreadPriority(THREAD_PRIORITY_NORMAL);
                    LogPrintf("ZcashMiner:\n");
                    LogPrintf("proof-of-work found  \n  hash: %s  \ntarget: %s\n", pblock->GetHash().GetHex(), hashTarget.GetHex());
                    if (ProcessBlockFound(pblock, chainparams)) {
                        // Ignore chain updates caused by us
                        std::lock_guard<std::mutex> lock{m_cs};
                        cancelSolver = false;
                    }
                    SetThreadPriority(THREAD_PRIORITY_LOWEST);
                    std::visit(KeepMinerAddress(), minerAddress);

                    // In regression test mode, stop mining after a block is found.
                    if (chainparams.MineBlocksOnDemand()) {
                        // Increment here because throwing skips the call below
                        ehSolverRuns.increment();
                        throw boost::thread_interrupted();
                    }

                    return true;
                };
                std::function<bool(EhSolverCancelCheck)> cancelled = [&m_cs, &cancelSolver](EhSolverCancelCheck pos) {
                    std::lock_guard<std::mutex> lock{m_cs};
                    return cancelSolver;
                };

                // TODO: factor this out into a function with the same API for each solver.
                if (solver == "tromp") {
                    // Create solver and initialize it.
                    equi eq(1);
                    eq.setstate(curr_state.inner);

                    // Initialization done, start algo driver.
                    eq.digit0(0);
                    eq.xfull = eq.bfull = eq.hfull = 0;
                    eq.showbsizes(0);
                    for (u32 r = 1; r < WK; r++) {
                        (r&1) ? eq.digitodd(r, 0) : eq.digiteven(r, 0);
                        eq.xfull = eq.bfull = eq.hfull = 0;
                        eq.showbsizes(r);
                    }
                    eq.digitK(0);
                    ehSolverRuns.increment();

                    // Convert solution indices to byte array (decompress) and pass it to validBlock method.
                    for (size_t s = 0; s < eq.nsols; s++) {
                        LogPrint("pow", "Checking solution %d\n", s+1);
                        std::vector<eh_index> index_vector(PROOFSIZE);
                        for (size_t i = 0; i < PROOFSIZE; i++) {
                            index_vector[i] = eq.sols[s][i];
                        }
                        std::vector<unsigned char> sol_char = GetMinimalFromIndices(index_vector, DIGITBITS);

                        if (validBlock(sol_char)) {
                            // If we find a POW solution, do not try other solutions
                            // because they become invalid as we created a new block in blockchain.
                            break;
                        }
                    }
                } else {
                    try {
                        // If we find a valid block, we rebuild
                        bool found = EhOptimisedSolve(n, k, curr_state, validBlock, cancelled);
                        ehSolverRuns.increment();
                        if (found) {
                            break;
                        }
                    } catch (EhSolverCancelledException&) {
                        LogPrint("pow", "Equihash solver cancelled\n");
                        std::lock_guard<std::mutex> lock{m_cs};
                        cancelSolver = false;
                    }
                }

                // Check for stop or if block needs to be rebuilt
                boost::this_thread::interruption_point();
                // Regtest mode doesn't require peers
                if (vNodes.empty() && chainparams.MiningRequiresPeers())
                    break;
                if ((UintToArith256(pblock->nNonce) & 0xffff) == 0xffff)
                    break;
                if (mempool.GetTransactionsUpdated() != nTransactionsUpdatedLast && GetTime() - nStart > 60)
                    break;
                if (pindexPrev != chainActive.Tip())
                    break;

                // Update nNonce and nTime
                pblock->nNonce = ArithToUint256(UintToArith256(pblock->nNonce) + 1);
                UpdateTime(pblock, chainparams.GetConsensus(), pindexPrev);
                if (chainparams.GetConsensus().nPowAllowMinDifficultyBlocksAfterHeight != std::nullopt)
                {
                    // Changing pblock->nTime can change work required on testnet:
                    hashTarget.SetCompact(pblock->nBits);
                }
            }
        }
    }
    catch (const boost::thread_interrupted&)
    {
        miningTimer.stop();
        c.disconnect();
        LogPrintf("ZcashMiner terminated\n");
        throw;
    }
    catch (const std::runtime_error &e)
    {
        miningTimer.stop();
        c.disconnect();
        LogPrintf("ZcashMiner runtime error: %s\n", e.what());
        return;
    }
    miningTimer.stop();
    c.disconnect();
}

void GenerateBitcoins(bool fGenerate, int nThreads, const CChainParams& chainparams)
{
    static boost::thread_group* minerThreads = NULL;

    if (nThreads < 0)
        nThreads = GetNumCores();

    if (minerThreads != NULL)
    {
        minerThreads->interrupt_all();
        minerThreads->join_all();
        delete minerThreads;
        minerThreads = NULL;
    }

    if (nThreads == 0 || !fGenerate)
        return;

    minerThreads = new boost::thread_group();
    for (int i = 0; i < nThreads; i++) {
        minerThreads->create_thread(boost::bind(&BitcoinMiner, boost::cref(chainparams)));
    }
}

#endif // ENABLE_MINING
