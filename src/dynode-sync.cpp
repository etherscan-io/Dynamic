// Copyright (c) 2014-2017 The Dash Core Developers
// Copyright (c) 2016-2017 Duality Blockchain Solutions Developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "dynode-sync.h"

#include "activedynode.h"
#include "checkpoints.h"
#include "governance.h"
#include "dynode.h"
#include "dynode-payments.h"
#include "dynodeman.h"
#include "main.h"
#include "netfulfilledman.h"
#include "spork.h"
#include "util.h"

class CDynodeSync;
CDynodeSync dynodeSync;

void CDynodeSync::Fail()
{
    nTimeLastFailure = GetTime();
    nRequestedDynodeAssets = DYNODE_SYNC_FAILED;
}

void CDynodeSync::Reset()
{
    nRequestedDynodeAssets = DYNODE_SYNC_INITIAL;
    nRequestedDynodeAttempt = 0;
    nTimeAssetSyncStarted = GetTime();
    nTimeLastBumped = 0;
    nTimeLastFailure = 0;
}

void CDynodeSync::BumpAssetLastTime(std::string strFuncName)
{
    if(IsSynced() || IsFailed()) return;
    nTimeLastBumped = GetTime();
    if(fDebug) LogPrintf("CDynodeSync::BumpAssetLastTime -- %s\n", strFuncName);
}

std::string CDynodeSync::GetAssetName()
{
    switch(nRequestedDynodeAssets)
    {
        case(DYNODE_SYNC_INITIAL):      return "DYNODE_SYNC_INITIAL";
        case(DYNODE_SYNC_LIST):         return "DYNODE_SYNC_LIST";
        case(DYNODE_SYNC_DNW):          return "DYNODE_SYNC_DNW";
        case(DYNODE_SYNC_GOVERNANCE):   return "DYNODE_SYNC_GOVERNANCE";
        case(DYNODE_SYNC_FAILED):       return "DYNODE_SYNC_FAILED";
        case DYNODE_SYNC_FINISHED:      return "DYNODE_SYNC_FINISHED";
        default:                           return "UNKNOWN";
    }
}

void CDynodeSync::SwitchToNextAsset()
{
    switch(nRequestedDynodeAssets)
    {
        case(DYNODE_SYNC_FAILED):
            throw std::runtime_error("Can't switch to next asset from failed, should use Reset() first!");
            break;
        case(DYNODE_SYNC_INITIAL):
            ClearFulfilledRequests();
            nRequestedDynodeAssets = DYNODE_SYNC_LIST;
            LogPrintf("CDynodeSync::SwitchToNextAsset -- Starting %s\n", GetAssetName());
            break;
        case(DYNODE_SYNC_LIST):
            LogPrintf("CDynodeSync::SwitchToNextAsset -- Completed %s in %llds\n", GetAssetName(), GetTime() - nTimeAssetSyncStarted);
            nRequestedDynodeAssets = DYNODE_SYNC_DNW;
            LogPrintf("CDynodeSync::SwitchToNextAsset -- Starting %s\n", GetAssetName());
            break;
        case(DYNODE_SYNC_DNW):
            LogPrintf("CDynodeSync::SwitchToNextAsset -- Completed %s in %llds\n", GetAssetName(), GetTime() - nTimeAssetSyncStarted);
            nRequestedDynodeAssets = DYNODE_SYNC_GOVERNANCE;
            LogPrintf("CDynodeSync::SwitchToNextAsset -- Starting %s\n", GetAssetName());
            break;
        case(DYNODE_SYNC_GOVERNANCE):
            LogPrintf("CDynodeSync::SwitchToNextAsset -- Completed %s in %llds\n", GetAssetName(), GetTime() - nTimeAssetSyncStarted);
            nRequestedDynodeAssets = DYNODE_SYNC_FINISHED;
            uiInterface.NotifyAdditionalDataSyncProgressChanged(1);
            //try to activate our dynode if possible
            activeDynode.ManageState();

            // TODO: Find out whether we can just use LOCK instead of:
            // TRY_LOCK(cs_vNodes, lockRecv);
            // if(lockRecv) { ... }

            g_connman->ForEachNode([](CNode* pnode) {
                netfulfilledman.AddFulfilledRequest(pnode->addr, "full-sync");
            });
            LogPrintf("CDynodeSync::SwitchToNextAsset -- Sync has finished\n");

            break;
    }
    nRequestedDynodeAttempt = 0;
    nTimeAssetSyncStarted = GetTime();
    BumpAssetLastTime("CDynodeSync::SwitchToNextAsset");
}

std::string CDynodeSync::GetSyncStatus()
{
    switch (dynodeSync.nRequestedDynodeAssets) {
        case DYNODE_SYNC_INITIAL:       return _("Synchronization pending...");
        case DYNODE_SYNC_LIST:          return _("Synchronizing Dynodes...");
        case DYNODE_SYNC_DNW:           return _("Synchronizing Dynode payments...");
        case DYNODE_SYNC_GOVERNANCE:    return _("Synchronizing governance objects...");
        case DYNODE_SYNC_FAILED:        return _("Synchronization failed");
        case DYNODE_SYNC_FINISHED:      return _("Synchronization finished");
        default:                           return "";
    }
}

void CDynodeSync::ProcessMessage(CNode* pfrom, std::string& strCommand, CDataStream& vRecv)
{
    if (strCommand == NetMsgType::SYNCSTATUSCOUNT) { //Sync status count

        //do not care about stats if sync process finished or failed
        if(IsSynced() || IsFailed()) return;

        int nItemID;
        int nCount;
        vRecv >> nItemID >> nCount;

        LogPrintf("SYNCSTATUSCOUNT -- got inventory count: nItemID=%d  nCount=%d  peer=%d\n", nItemID, nCount, pfrom->id);
    }
}

void CDynodeSync::ClearFulfilledRequests()
{
    // TODO: Find out whether we can just use LOCK instead of:
    // TRY_LOCK(cs_vNodes, lockRecv);
    // if(!lockRecv) return;

    g_connman->ForEachNode([](CNode* pnode) {
        netfulfilledman.RemoveFulfilledRequest(pnode->addr, "spork-sync");
        netfulfilledman.RemoveFulfilledRequest(pnode->addr, "dynode-list-sync");
        netfulfilledman.RemoveFulfilledRequest(pnode->addr, "dynode-payment-sync");
        netfulfilledman.RemoveFulfilledRequest(pnode->addr, "governance-sync");
        netfulfilledman.RemoveFulfilledRequest(pnode->addr, "full-sync");
    });
}

void CDynodeSync::ProcessTick()
{
    static int nTick = 0;

    if(nTick++ % DYNODE_SYNC_TICK_SECONDS != 0) return;
    
    if(!pCurrentBlockIndex) return;

    // reset the sync process if the last call to this function was more than 60 minutes ago (client was in sleep mode)
    static int64_t nTimeLastProcess = GetTime();
    if(GetTime() - nTimeLastProcess > 60*60) {
        LogPrintf("CDynodeSync::HasSyncFailures -- WARNING: no actions for too long, restarting sync...\n");
        Reset();
        SwitchToNextAsset();
        return;
    }
    nTimeLastProcess = GetTime();

    // reset sync status in case of any other sync failure
    if(IsFailed()) {
        if(nTimeLastFailure + (1*60) < GetTime()) { // 1 minute cooldown after failed sync
            LogPrintf("CDynodeSync::HasSyncFailures -- WARNING: failed to sync, trying again...\n");
            Reset();
            SwitchToNextAsset();
        }
        return;
    }

    // gradually request the rest of the votes after sync finished
    if(IsSynced()) {
        std::vector<CNode*> vNodesCopy = g_connman->CopyNodeVector();
        governance.RequestGovernanceObjectVotes(vNodesCopy);
        g_connman->ReleaseNodeVector(vNodesCopy);
        return;
    }

    // Calculate "progress" for LOG reporting / GUI notification
    double nSyncProgress = double(nRequestedDynodeAttempt + (nRequestedDynodeAssets - 1) * 8) / (8*4);
    LogPrintf("CDynodeSync::ProcessTick -- nTick %d nRequestedDynodeAssets %d nRequestedDynodeAttempt %d nSyncProgress %f\n", nTick, nRequestedDynodeAssets, nRequestedDynodeAttempt, nSyncProgress);
    uiInterface.NotifyAdditionalDataSyncProgressChanged(nSyncProgress);

    std::vector<CNode*> vNodesCopy = g_connman->CopyNodeVector();

    BOOST_FOREACH(CNode* pnode, vNodesCopy)    {
        // Don't try to sync any data from outbound "dynode" connections -
        // they are temporary and should be considered unreliable for a sync process.
        // Inbound connection this early is most likely a "dynode" connection
        // initialted from another node, so skip it too.
        if(pnode->fDynode || (fDyNode && pnode->fInbound)) continue;
        // QUICK MODE (REGTEST ONLY!)
        if(Params().NetworkIDString() == CBaseChainParams::REGTEST)
        {
            if(nRequestedDynodeAttempt <= 2) {
                pnode->PushMessage(NetMsgType::GETSPORKS); //get current network sporks
            } else if(nRequestedDynodeAttempt < 4) {
                dnodeman.SsegUpdate(pnode);
            } else if(nRequestedDynodeAttempt < 6) {
                int nDnCount = dnodeman.CountDynodes();
                pnode->PushMessage(NetMsgType::DYNODEPAYMENTSYNC, nDnCount); //sync payment votes
                SendGovernanceSyncRequest(pnode);
            } else {
                nRequestedDynodeAssets = DYNODE_SYNC_FINISHED;
            }
            nRequestedDynodeAttempt++;
            g_connman->ReleaseNodeVector(vNodesCopy);
            return;
        }

        // NORMAL NETWORK MODE - TESTNET/MAINNET
        {
            if(netfulfilledman.HasFulfilledRequest(pnode->addr, "full-sync")) {
                // We already fully synced from this node recently,
                // disconnect to free this connection slot for another peer.
                pnode->fDisconnect = true;
                LogPrintf("CDynodeSync::ProcessTick -- disconnecting from recently synced peer %d\n", pnode->id);
                continue;
            }

            // SPORK : ALWAYS ASK FOR SPORKS AS WE SYNC

            if(!netfulfilledman.HasFulfilledRequest(pnode->addr, "spork-sync")) {
                // always get sporks first, only request once from each peer
                netfulfilledman.AddFulfilledRequest(pnode->addr, "spork-sync");
                // get current network sporks
                pnode->PushMessage(NetMsgType::GETSPORKS);
                LogPrintf("CDynodeSync::ProcessTick -- nTick %d nRequestedDynodeAssets %d -- requesting sporks from peer %d\n", nTick, nRequestedDynodeAssets, pnode->id);
            }

            // MNLIST : SYNC DYNODE LIST FROM OTHER CONNECTED CLIENTS

            if(nRequestedDynodeAssets == DYNODE_SYNC_LIST) {
                LogPrint("dynode", "CDynodeSync::ProcessTick -- nTick %d nRequestedDynodeAssets %d nTimeLastBumped %lld GetTime() %lld diff %lld\n", nTick, nRequestedDynodeAssets, nTimeLastBumped, GetTime(), GetTime() - nTimeLastBumped);
                // check for timeout first
                if(GetTime() - nTimeLastBumped > DYNODE_SYNC_TIMEOUT_SECONDS) {
                    LogPrintf("CDynodeSync::ProcessTick -- nTick %d nRequestedDynodeAssets %d -- timeout\n", nTick, nRequestedDynodeAssets);
                    if (nRequestedDynodeAttempt == 0) {
                        LogPrintf("CDynodeSync::ProcessTick -- ERROR: failed to sync %s\n", GetAssetName());
                        // there is no way we can continue without Dynode list, fail here and try later
                        Fail();
                        g_connman->ReleaseNodeVector(vNodesCopy);
                        return;
                    }
                    SwitchToNextAsset();
                    g_connman->ReleaseNodeVector(vNodesCopy);
                    return;
                }

                // only request once from each peer
                if(netfulfilledman.HasFulfilledRequest(pnode->addr, "dynode-list-sync")) continue;
                netfulfilledman.AddFulfilledRequest(pnode->addr, "dynode-list-sync");

                if (pnode->nVersion < dnpayments.GetMinDynodePaymentsProto()) continue;
                nRequestedDynodeAttempt++;

                dnodeman.SsegUpdate(pnode);

                g_connman->ReleaseNodeVector(vNodesCopy);
                return; //this will cause each peer to get one request each six seconds for the various assets we need
            }

            // DNW : SYNC DYNODE PAYMENT VOTES FROM OTHER CONNECTED CLIENTS

            if(nRequestedDynodeAssets == DYNODE_SYNC_DNW) {
                LogPrint("dnpayments", "CDynodeSync::ProcessTick -- nTick %d nRequestedDynodeAssets %d nTimeLastBumped %lld GetTime() %lld diff %lld\n", nTick, nRequestedDynodeAssets, nTimeLastBumped, GetTime(), GetTime() - nTimeLastBumped);
                // check for timeout first
                // This might take a lot longer than DYNODE_SYNC_TIMEOUT_SECONDS due to new blocks,
                // but that should be OK and it should timeout eventually.
                if(GetTime() - nTimeLastBumped > DYNODE_SYNC_TIMEOUT_SECONDS) {
                    LogPrintf("CDynodeSync::ProcessTick -- nTick %d nRequestedDynodeAssets %d -- timeout\n", nTick, nRequestedDynodeAssets);
                    if (nRequestedDynodeAttempt == 0) {
                        LogPrintf("CDynodeSync::ProcessTick -- ERROR: failed to sync %s\n", GetAssetName());
                        // probably not a good idea to proceed without winner list
                        Fail();
                        g_connman->ReleaseNodeVector(vNodesCopy);
                        return;
                    }
                    SwitchToNextAsset();
                    g_connman->ReleaseNodeVector(vNodesCopy);
                    return;
                }
                // check for data
                // if dnpayments already has enough blocks and votes, switch to the next asset
                // try to fetch data from at least two peers though
                if(nRequestedDynodeAttempt > 1 && dnpayments.IsEnoughData()) {
                    LogPrintf("CDynodeSync::ProcessTick -- nTick %d nRequestedDynodeAssets %d -- found enough data\n", nTick, nRequestedDynodeAssets);
                    SwitchToNextAsset();
                    g_connman->ReleaseNodeVector(vNodesCopy);
                    return;
                }

                // only request once from each peer
                if(netfulfilledman.HasFulfilledRequest(pnode->addr, "dynode-payment-sync")) continue;
                netfulfilledman.AddFulfilledRequest(pnode->addr, "dynode-payment-sync");

                if(pnode->nVersion < dnpayments.GetMinDynodePaymentsProto()) continue;
                nRequestedDynodeAttempt++;

                // ask node for all payment votes it has (new nodes will only return votes for future payments)
                pnode->PushMessage(NetMsgType::DYNODEPAYMENTSYNC, dnpayments.GetStorageLimit());
                // ask node for missing pieces only (old nodes will not be asked)
                dnpayments.RequestLowDataPaymentBlocks(pnode);

                g_connman->ReleaseNodeVector(vNodesCopy);
                return; //this will cause each peer to get one request each six seconds for the various assets we need
            }

            // GOVOBJ : SYNC GOVERNANCE ITEMS FROM OUR PEERS

            if(nRequestedDynodeAssets == DYNODE_SYNC_GOVERNANCE) {
                LogPrint("gobject", "CDynodeSync::ProcessTick -- nTick %d nRequestedDynodeAssets %d nTimeLastBumped %lld GetTime() %lld diff %lld\n", nTick, nRequestedDynodeAssets, nTimeLastBumped, GetTime(), GetTime() - nTimeLastBumped);

                // check for timeout first
                if(GetTime() - nTimeLastBumped > DYNODE_SYNC_TIMEOUT_SECONDS) {
                    LogPrintf("CDynodeSync::ProcessTick -- nTick %d nRequestedDynodeAssets %d -- timeout\n", nTick, nRequestedDynodeAssets);
                    if(nRequestedDynodeAttempt == 0) {
                        LogPrintf("CDynodeSync::ProcessTick -- WARNING: failed to sync %s\n", GetAssetName());
                        // it's kind of ok to skip this for now, hopefully we'll catch up later?
                    }
                    SwitchToNextAsset();
                    g_connman->ReleaseNodeVector(vNodesCopy);
                    return;
                }
                // only request obj sync once from each peer, then request votes on per-obj basis
                if(netfulfilledman.HasFulfilledRequest(pnode->addr, "governance-sync")) {
                    governance.RequestGovernanceObjectVotes(pnode);
                    int nObjsLeftToAsk = governance.RequestGovernanceObjectVotes(pnode);
                    static int64_t nTimeNoObjectsLeft = 0;
                    // check for data
                    if(nObjsLeftToAsk == 0) {
                        static int nLastTick = 0;
                        static int nLastVotes = 0;
                        if(nTimeNoObjectsLeft == 0) {
                            // asked all objects for votes for the first time
                            nTimeNoObjectsLeft = GetTime();
                        }
                        // make sure the condition below is checked only once per tick
                        if(nLastTick == nTick) continue;

                        if(GetTime() - nTimeNoObjectsLeft > DYNODE_SYNC_TIMEOUT_SECONDS &&
                            governance.GetVoteCount() - nLastVotes < std::max(int(0.0001 * nLastVotes), DYNODE_SYNC_TICK_SECONDS)
                        ) {
                            // We already asked for all objects, waited for DYNODE_SYNC_TIMEOUT_SECONDS
                            // after that and less then 0.01% or DYNODE_SYNC_TICK_SECONDS
                            // (i.e. 1 per second) votes were recieved during the last tick.
                            // We can be pretty sure that we are done syncing.
                            LogPrintf("CDynodeSync::ProcessTick -- nTick %d nRequestedDynodeAssets %d -- asked for all objects, nothing to do\n", nTick, nRequestedDynodeAssets);
                            // reset nTimeNoObjectsLeft to be able to use the same condition on resync
                            nTimeNoObjectsLeft = 0;
                            SwitchToNextAsset();
                            g_connman->ReleaseNodeVector(vNodesCopy);
                            return;
                        }

                        nLastTick = nTick;
                        nLastVotes = governance.GetVoteCount();
                    }
                 }

                netfulfilledman.AddFulfilledRequest(pnode->addr, "governance-sync");

                if (pnode->nVersion < MIN_GOVERNANCE_PEER_PROTO_VERSION) continue;
                nRequestedDynodeAttempt++;

                SendGovernanceSyncRequest(pnode);

                g_connman->ReleaseNodeVector(vNodesCopy);
                return; //this will cause each peer to get one request each six seconds for the various assets we need
            }
        }
    }
    // looped through all nodes, release them
    g_connman->ReleaseNodeVector(vNodesCopy);
}

void CDynodeSync::SendGovernanceSyncRequest(CNode* pnode)
{
    CBloomFilter filter;
    filter.clear();

    pnode->PushMessage(NetMsgType::DNGOVERNANCESYNC, uint256(), filter);
}

void CDynodeSync::UpdatedBlockTip(const CBlockIndex *pindexNew, bool fInitialDownload)
{
    pCurrentBlockIndex = pindexNew;
    if(fDebug) LogPrintf("CDynodeSync::UpdatedBlockTip -- pCurrentBlockIndex->nHeight: %d fInitialDownload=%d\n", pCurrentBlockIndex->nHeight, fInitialDownload);
    // nothing to do here if we failed to sync previousely,
    // just wait till status reset after a cooldown (see ProcessTick)
    if(IsFailed()) return;
    // switch from DYNODE_SYNC_INITIAL to the next "asset"
    // the first time we are out of IBD mode (and only the first time)
    if(!fInitialDownload && !IsBlockchainSynced()) SwitchToNextAsset();
    // postpone timeout each time new block arrives while we are syncing
    if(!IsSynced()) BumpAssetLastTime("CDynodeSync::UpdatedBlockTip");
}
