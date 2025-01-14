// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2018 The Bitcoin Core developers
// Copyright (c) 2020 The Browncoin Core developers The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pow.h>

#include <arith_uint256.h>
#include <chain.h>
#include <chainparams.h>
#include <primitives/block.h>
#include <uint256.h>
#include <bignum.h>
#include <logging.h>
#include <crypto/verthash.h>

unsigned int GetNextWorkRequired(const CBlockIndex* pindexLast, const CBlockHeader *pblock, const Consensus::Params& params)
{
    static CBigNum              bnProofOfWorkLimit(params.powLimit);
    static CBigNum              bnPreVerthashProofOfWorkLimit(params.preVerthashPowLimit);
    static const int64_t        BlocksTargetSpacing  = 1 * 60; // 1 minute
    unsigned int                TimeDaySeconds       = 60 * 60 * 24;
    int64_t                     PastSecondsMin       = TimeDaySeconds * 0.25;
    int64_t                     PastSecondsMax       = TimeDaySeconds * 7;
    uint64_t                    PastBlocksMin        = PastSecondsMin / BlocksTargetSpacing;
    uint64_t                    PastBlocksMax        = PastSecondsMax / BlocksTargetSpacing;

    const int nHeight = pindexLast->nHeight + 1;

    if(Params().NetworkIDString() == CBaseChainParams::MAIN) {
        if(nHeight < VERTHASH_FORKBLOCK_MAINNET) {
            return GetNextWorkRequired_Scrypt(pindexLast, pblock, params);
        } else if(nHeight >= VERTHASH_FORKBLOCK_MAINNET && nHeight < VERTHASH_FORKBLOCK_MAINNET+10) { // Force difficulty for 10 blocks - Verthash hardfork
            return 0x1d00fff6;
        }
    } else {
        if(nHeight < VERTHASH_FORKBLOCK_TESTNET) {
            return GetNextWorkRequired_Scrypt(pindexLast, pblock, params);
        } else if(nHeight >= VERTHASH_FORKBLOCK_TESTNET && nHeight < VERTHASH_FORKBLOCK_TESTNET+10) { // Set diff to mindiff on testnet Verthash fork
            return bnProofOfWorkLimit.GetCompact();
        }

        if(nHeight % 12 != 0) {
            CBigNum bnNew;
	        bnNew.SetCompact(pindexLast->nBits);
	        if (bnNew > bnProofOfWorkLimit) { bnNew = bnProofOfWorkLimit; }
            return bnNew.GetCompact();
        }
    }
    return KimotoGravityWell(pindexLast, pblock, BlocksTargetSpacing, PastBlocksMin, PastBlocksMax, params);
}

unsigned int GetNextWorkRequired_Scrypt(const CBlockIndex* pindexLast, const CBlockHeader *pblock, const Consensus::Params& params)
{
    assert(pindexLast != nullptr);
    unsigned int nProofOfWorkLimit = UintToArith256(params.preVerthashPowLimit).GetCompact();

    // Only change once per difficulty adjustment interval
    if ((pindexLast->nHeight+1) % params.preDifficultyAdjustmentInterval() != 0)
    {
        if (params.fPowAllowMinDifficultyBlocks)
        {
            // Special difficulty rule for testnet:
            // If the new block's timestamp is more than 2* 10 minutes
            // then allow mining of a min-difficulty block.
            if (pblock->GetBlockTime() > pindexLast->GetBlockTime() + params.nPowTargetSpacing*2)
                return nProofOfWorkLimit;
            else
            {
                // Return the last non-special-min-difficulty-rules-block
                const CBlockIndex* pindex = pindexLast;
                while (pindex->pprev && pindex->nHeight % params.preDifficultyAdjustmentInterval() != 0 && pindex->nBits == nProofOfWorkLimit)
                    pindex = pindex->pprev;
                return pindex->nBits;
            }
        }
        return pindexLast->nBits;
    }

    // Go back by what we want to be 14 days worth of blocks
    // Browncoin: This fixes an issue where a 51% attack can change difficulty at will.
    // Go back the full period unless it's the first retarget after genesis. Code courtesy of Art Forz
    int blockstogoback = params.preDifficultyAdjustmentInterval()-1;
    if ((pindexLast->nHeight+1) != params.preDifficultyAdjustmentInterval())
        blockstogoback = params.preDifficultyAdjustmentInterval();

    // Go back by what we want to be 14 days worth of blocks
    const CBlockIndex* pindexFirst = pindexLast;
    for (int i = 0; pindexFirst && i < blockstogoback; i++)
        pindexFirst = pindexFirst->pprev;

    assert(pindexFirst);

    return CalculateNextWorkRequired(pindexLast, pindexFirst->GetBlockTime(), params);
}

unsigned int KimotoGravityWell(const CBlockIndex* pindexLast,
                               const CBlockHeader *pblock,
                               uint64_t TargetBlocksSpacingSeconds,
                               uint64_t PastBlocksMin,
                               uint64_t PastBlocksMax,
                               const Consensus::Params& params) {
    /* current difficulty formula - kimoto gravity well */
    const CBlockIndex *BlockLastSolved                                = pindexLast;
    const CBlockIndex *BlockReading                                = pindexLast;
    uint64_t                                PastBlocksMass                                = 0;
    int64_t                                PastRateActualSeconds                = 0;
    int64_t                                PastRateTargetSeconds                = 0;
    double                                PastRateAdjustmentRatio                = double(1);
    CBigNum                                PastDifficultyAverage;
    CBigNum                                PastDifficultyAveragePrev;
    double                                EventHorizonDeviation;
    double                                EventHorizonDeviationFast;
    double                                EventHorizonDeviationSlow;
    static CBigNum                          bnProofOfWorkLimit(params.powLimit);
    static CBigNum                          bnPreVerthashProofOfWorkLimit(params.preVerthashPowLimit);

    if (BlockLastSolved == NULL || BlockLastSolved->nHeight == 0 || static_cast<uint64_t>(BlockLastSolved->nHeight) < PastBlocksMin) { return UintToArith256(params.powLimit).GetCompact(); }

    for (unsigned int i = 1; BlockReading && BlockReading->nHeight > 0; i++) {
        if (PastBlocksMax > 0 && i > PastBlocksMax) { break; }
        PastBlocksMass++;

        if (i == 1) {
            PastDifficultyAverage.SetCompact(BlockReading->nBits);
        } else {
            PastDifficultyAverage = ((CBigNum().SetCompact(BlockReading->nBits) - PastDifficultyAveragePrev) / i) + PastDifficultyAveragePrev;
        }
        PastDifficultyAveragePrev = PastDifficultyAverage;

        PastRateActualSeconds                        = BlockLastSolved->GetBlockTime() - BlockReading->GetBlockTime();
        PastRateTargetSeconds                        = TargetBlocksSpacingSeconds * PastBlocksMass;
        PastRateAdjustmentRatio                        = double(1);
        if (PastRateActualSeconds < 0) { PastRateActualSeconds = 0; }
        if (PastRateActualSeconds != 0 && PastRateTargetSeconds != 0) {
            PastRateAdjustmentRatio                        = double(PastRateTargetSeconds) / double(PastRateActualSeconds);
        }
        EventHorizonDeviation                        = 1 + (0.7084 * std::pow((double(PastBlocksMass)/double(144)), -1.228));
        EventHorizonDeviationFast                = EventHorizonDeviation;
        EventHorizonDeviationSlow                = 1 / EventHorizonDeviation;

        if (PastBlocksMass >= PastBlocksMin) {
                if ((PastRateAdjustmentRatio <= EventHorizonDeviationSlow) || (PastRateAdjustmentRatio >= EventHorizonDeviationFast)) { assert(BlockReading); break; }
        }
        if (BlockReading->pprev == NULL ||
            (Params().NetworkIDString() == CBaseChainParams::MAIN && (BlockReading->nHeight == VERTHASH_FORKBLOCK_MAINNET))) // Don't calculate past fork block on mainnet
        {
                assert(BlockReading);
                break;
        }
        BlockReading = BlockReading->pprev;
    }

    CBigNum bnNew(PastDifficultyAverage);
    if (PastRateActualSeconds != 0 && PastRateTargetSeconds != 0) {
            bnNew *= PastRateActualSeconds;
            bnNew /= PastRateTargetSeconds;
    }

    if(Params().NetworkIDString() == CBaseChainParams::MAIN) {
        if(static_cast<uint64_t>(BlockLastSolved->nHeight)+1 >= VERTHASH_FORKBLOCK_MAINNET) {
            if (bnNew > bnProofOfWorkLimit) {
                return bnProofOfWorkLimit.GetCompact();
            }
        } else if (bnNew > bnPreVerthashProofOfWorkLimit) {
            return bnPreVerthashProofOfWorkLimit.GetCompact();
        }
    } else if(Params().NetworkIDString() == CBaseChainParams::TESTNET) {
        if(static_cast<uint64_t>(BlockLastSolved->nHeight)+1 >= VERTHASH_FORKBLOCK_TESTNET) {
            if (bnNew > bnProofOfWorkLimit) {
                return bnProofOfWorkLimit.GetCompact();
            }
        } else if (bnNew > bnPreVerthashProofOfWorkLimit) {
            return bnPreVerthashProofOfWorkLimit.GetCompact();
        }
    } else if (bnNew > bnProofOfWorkLimit) { // REGTEST
        bnNew = bnProofOfWorkLimit;
	}

    return bnNew.GetCompact();
}

unsigned int CalculateNextWorkRequired(const CBlockIndex* pindexLast, int64_t nFirstBlockTime, const Consensus::Params& params)
{
    if (params.fPowNoRetargeting)
        return pindexLast->nBits;

    // Limit adjustment step
    int64_t nActualTimespan = pindexLast->GetBlockTime() - nFirstBlockTime;
    if (nActualTimespan < params.nPreKGWPowTargetTimespan/4)
        nActualTimespan = params.nPreKGWPowTargetTimespan/4;
    if (nActualTimespan > params.nPreKGWPowTargetTimespan*4)
        nActualTimespan = params.nPreKGWPowTargetTimespan*4;

    // Retarget
    arith_uint256 bnNew;
    arith_uint256 bnOld;
    bnNew.SetCompact(pindexLast->nBits);
    bnOld = bnNew;
    // Browncoin: intermediate uint256 can overflow by 1 bit
    const arith_uint256 bnPowLimit = UintToArith256(params.preVerthashPowLimit);
    bool fShift = bnNew.bits() > bnPowLimit.bits() - 1;
    if (fShift)
        bnNew >>= 1;
    bnNew *= nActualTimespan;
    bnNew /= params.nPreKGWPowTargetTimespan;
    if (fShift)
        bnNew <<= 1;

    if (bnNew > bnPowLimit)
        bnNew = bnPowLimit;

    return bnNew.GetCompact();
}

bool CheckProofOfWork(uint256 hash, unsigned int nBits, const Consensus::Params& params)
{
    bool fNegative;
    bool fOverflow;
    arith_uint256 bnTarget;

    bnTarget.SetCompact(nBits, &fNegative, &fOverflow);

    // Check range
    if (fNegative || bnTarget == 0 || fOverflow || bnTarget > UintToArith256(params.powLimit))
        return false;

    // Check proof of work matches claimed amount
    if (UintToArith256(hash) > bnTarget)
        return false;

    return true;
}
