// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2012 The Bitcoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.


#include "anonsend.h"
#include "main.h"
#include "init.h"
//#include "script/sign.h"
#include "util.h"
#include "inode.h"
#include "tesseractx.h"
#include "ui_interface.h"
//#include "random.h"

#include <openssl/rand.h>

#include <boost/algorithm/string/replace.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/lexical_cast.hpp>

#include <algorithm>
#include <boost/assign/list_of.hpp>

using namespace std;
using namespace boost;

CCriticalSection cs_anonsend;

/** The main object for accessing anonsend */
CAnonSendPool anonSendPool;
/** A helper object for signing messages from inodes */
CAnonSendSigner anonSendSigner;
/** The current anonsends in progress on the network */
std::vector<CAnonsendQueue> vecAnonsendQueue;
/** Keep track of the used inodes */
std::vector<CTxIn> vecInodesUsed;
// keep track of the scanning errors I've seen
map<uint256, CAnonsendBroadcastTx> mapAnonsendBroadcastTxes;
//
CActiveInode activeInode;

// count peers we've requested the list from
int RequestedINodeList = 0;

void ProcessMessageAnonsend(CNode* pfrom, std::string& strCommand, CDataStream& vRecv)
{
    if(fLiteMode) return; //disable all anonsend/inode related functionality

    if (strCommand == "dsf") { //AnonSend Final tx
        if (pfrom->nVersion < anonSendPool.MIN_PEER_PROTO_VERSION) {
            return;
        }

        if((CNetAddr)anonSendPool.submittedToInode != (CNetAddr)pfrom->addr){
            //LogPrintf("dsc - message doesn't match current inode - %s != %s\n", anonSendPool.submittedToInode.ToString().c_str(), pfrom->addr.ToString().c_str());
            return;
        }

        int sessionID;
        CTransaction txNew;
        vRecv >> sessionID >> txNew;

        if(anonSendPool.sessionID != sessionID){
            if (fDebug) LogPrintf("dsf - message doesn't match current anonsend session %d %d\n", anonSendPool.sessionID, sessionID);
            return;
        }

        //check to see if input is spent already? (and probably not confirmed)
        anonSendPool.SignFinalTransaction(txNew, pfrom);
    }

    else if (strCommand == "dsc") { //AnonSend Complete
        if (pfrom->nVersion < anonSendPool.MIN_PEER_PROTO_VERSION) {
            return;
        }

        if((CNetAddr)anonSendPool.submittedToInode != (CNetAddr)pfrom->addr){
            //LogPrintf("dsc - message doesn't match current inode - %s != %s\n", anonSendPool.submittedToInode.ToString().c_str(), pfrom->addr.ToString().c_str());
            return;
        }

        int sessionID;
        bool error;
        std::string lastMessage;
        vRecv >> sessionID >> error >> lastMessage;

        if(anonSendPool.sessionID != sessionID){
            if (fDebug) LogPrintf("dsc - message doesn't match current anonsend session %d %d\n", anonSendPool.sessionID, sessionID);
            return;
        }

        anonSendPool.CompletedTransaction(error, lastMessage);
    }

    else if (strCommand == "dsa") { //AnonSend Acceptable

        if (pfrom->nVersion < anonSendPool.MIN_PEER_PROTO_VERSION) {
            std::string strError = _("Incompatible version.");
            LogPrintf("dsa -- incompatible version! \n");
            pfrom->PushMessage("dssu", anonSendPool.sessionID, anonSendPool.GetState(), anonSendPool.GetEntriesCount(), INODE_REJECTED, strError);

            return;
        }

        if(!fINode){
            std::string strError = _("This is not a inode.");
            LogPrintf("dsa -- not a inode! \n");
            pfrom->PushMessage("dssu", anonSendPool.sessionID, anonSendPool.GetState(), anonSendPool.GetEntriesCount(), INODE_REJECTED, strError);

            return;
        }

        int nDenom;
        CTransaction txCollateral;
        vRecv >> nDenom >> txCollateral;

        std::string error = "";
        int mn = GetInodeByVin(activeInode.vin);
        if(mn == -1){
            std::string strError = _("Not in the inode list.");
            pfrom->PushMessage("dssu", anonSendPool.sessionID, anonSendPool.GetState(), anonSendPool.GetEntriesCount(), INODE_REJECTED, strError);
            return;
        }

        if(anonSendPool.sessionUsers == 0) {
            if(vecInodes[mn].nLastDsq != 0 &&
                vecInodes[mn].nLastDsq + CountInodesAboveProtocol(anonSendPool.MIN_PEER_PROTO_VERSION)/5 > anonSendPool.nDsqCount){
                //LogPrintf("dsa -- last dsq too recent, must wait. %s \n", vecInodes[mn].addr.ToString().c_str());
                std::string strError = _("Last Anonsend was too recent.");
                pfrom->PushMessage("dssu", anonSendPool.sessionID, anonSendPool.GetState(), anonSendPool.GetEntriesCount(), INODE_REJECTED, strError);
                return;
            }
        }

        if(!anonSendPool.IsCompatibleWithSession(nDenom, txCollateral, error))
        {
            LogPrintf("dsa -- not compatible with existing transactions! \n");
            pfrom->PushMessage("dssu", anonSendPool.sessionID, anonSendPool.GetState(), anonSendPool.GetEntriesCount(), INODE_REJECTED, error);
            return;
        } else {
            LogPrintf("dsa -- is compatible, please submit! \n");
            pfrom->PushMessage("dssu", anonSendPool.sessionID, anonSendPool.GetState(), anonSendPool.GetEntriesCount(), INODE_ACCEPTED, error);
            return;
        }
    } else if (strCommand == "dsq") { //AnonSend Queue

        if (pfrom->nVersion < anonSendPool.MIN_PEER_PROTO_VERSION) {
            return;
        }

        CAnonsendQueue dsq;
        vRecv >> dsq;


        CService addr;
        if(!dsq.GetAddress(addr)) return;
        if(!dsq.CheckSignature()) return;

        if(dsq.IsExpired()) return;

        int mn = GetInodeByVin(dsq.vin);
        if(mn == -1) return;

        // if the queue is ready, submit if we can
        if(dsq.ready) {
            if((CNetAddr)anonSendPool.submittedToInode != (CNetAddr)addr){
                LogPrintf("dsq - message doesn't match current inode - %s != %s\n", anonSendPool.submittedToInode.ToString().c_str(), pfrom->addr.ToString().c_str());
                return;
            }

            if (fDebug)  LogPrintf("anonsend queue is ready - %s\n", addr.ToString().c_str());
            anonSendPool.PrepareAnonsendDenominate();
        } else {
            BOOST_FOREACH(CAnonsendQueue q, vecAnonsendQueue){
                if(q.vin == dsq.vin) return;
            }

            if(fDebug) LogPrintf("dsq last %d last2 %d count %d\n", vecInodes[mn].nLastDsq, vecInodes[mn].nLastDsq + (int)vecInodes.size()/5, anonSendPool.nDsqCount);
            //don't allow a few nodes to dominate the queuing process
            if(vecInodes[mn].nLastDsq != 0 &&
                vecInodes[mn].nLastDsq + CountInodesAboveProtocol(anonSendPool.MIN_PEER_PROTO_VERSION)/5 > anonSendPool.nDsqCount){
                if(fDebug) LogPrintf("dsq -- inode sending too many dsq messages. %s \n", vecInodes[mn].addr.ToString().c_str());
                return;
            }
            anonSendPool.nDsqCount++;
            vecInodes[mn].nLastDsq = anonSendPool.nDsqCount;
            vecInodes[mn].allowFreeTx = true;

            if(fDebug) LogPrintf("dsq - new anonsend queue object - %s\n", addr.ToString().c_str());
            vecAnonsendQueue.push_back(dsq);
            dsq.Relay();
            dsq.time = GetTime();
        }

    } else if (strCommand == "dsi") { //AnonSend vIn
        std::string error = "";
        if (pfrom->nVersion < anonSendPool.MIN_PEER_PROTO_VERSION) {
            LogPrintf("dsi -- incompatible version! \n");
            error = _("Incompatible version.");
            pfrom->PushMessage("dssu", anonSendPool.sessionID, anonSendPool.GetState(), anonSendPool.GetEntriesCount(), INODE_REJECTED, error);

            return;
        }

        if(!fINode){
            LogPrintf("dsi -- not a inode! \n");
            error = _("This is not a inode.");
            pfrom->PushMessage("dssu", anonSendPool.sessionID, anonSendPool.GetState(), anonSendPool.GetEntriesCount(), INODE_REJECTED, error);

            return;
        }

        std::vector<CTxIn> in;
        int64_t nAmount;
        CTransaction txCollateral;
        std::vector<CTxOut> out;
        vRecv >> in >> nAmount >> txCollateral >> out;

        //do we have enough users in the current session?
        if(!anonSendPool.IsSessionReady()){
            LogPrintf("dsi -- session not complete! \n");
            error = _("Session not complete!");
            pfrom->PushMessage("dssu", anonSendPool.sessionID, anonSendPool.GetState(), anonSendPool.GetEntriesCount(), INODE_REJECTED, error);
            return;
        }

        //do we have the same denominations as the current session?
        if(!anonSendPool.IsCompatibleWithEntries(out))
        {
            LogPrintf("dsi -- not compatible with existing transactions! \n");
            error = _("Not compatible with existing transactions.");
            pfrom->PushMessage("dssu", anonSendPool.sessionID, anonSendPool.GetState(), anonSendPool.GetEntriesCount(), INODE_REJECTED, error);
            return;
        }

        //check it like a transaction
        {
            int64_t nValueIn = 0;
            int64_t nValueOut = 0;
            bool missingTx = false;

            CValidationState state;
            CTransaction tx;

            BOOST_FOREACH(CTxOut o, out){
                nValueOut += o.nValue;
                tx.vout.push_back(o);

                if(o.scriptPubKey.size() != 25){
                    LogPrintf("dsi - non-standard pubkey detected! %s\n", o.scriptPubKey.ToString().c_str());
                    error = _("Non-standard public key detected.");
                    pfrom->PushMessage("dssu", anonSendPool.sessionID, anonSendPool.GetState(), anonSendPool.GetEntriesCount(), INODE_REJECTED, error);
                    return;
                }
                if(!o.scriptPubKey.IsNormalPaymentScript()){
                    LogPrintf("dsi - invalid script! %s\n", o.scriptPubKey.ToString().c_str());
                    error = _("Invalid script detected.");
                    pfrom->PushMessage("dssu", anonSendPool.sessionID, anonSendPool.GetState(), anonSendPool.GetEntriesCount(), INODE_REJECTED, error);
                    return;
                }
            }

            BOOST_FOREACH(const CTxIn i, in){
                tx.vin.push_back(i);

                if(fDebug) LogPrintf("dsi -- tx in %s\n", i.ToString().c_str());

                CTransaction tx2;
                uint256 hash;
                //if(GetTransaction(i.prevout.hash, tx2, hash, true)){
		if(GetTransaction(i.prevout.hash, tx2, hash)){
                    if(tx2.vout.size() > i.prevout.n) {
                        nValueIn += tx2.vout[i.prevout.n].nValue;
                    }
                } else{
                    missingTx = true;
                }
            }

            if (nValueIn > ANONSEND_POOL_MAX) {
                LogPrintf("dsi -- more than anonsend pool max! %s\n", tx.ToString().c_str());
                error = _("Value more than Anonsend pool maximum allows.");
                pfrom->PushMessage("dssu", anonSendPool.sessionID, anonSendPool.GetState(), anonSendPool.GetEntriesCount(), INODE_REJECTED, error);
                return;
            }

            if(!missingTx){
                if (nValueIn-nValueOut > nValueIn*.01) {
                    LogPrintf("dsi -- fees are too high! %s\n", tx.ToString().c_str());
                    error = _("Transaction fees are too high.");
                    pfrom->PushMessage("dssu", anonSendPool.sessionID, anonSendPool.GetState(), anonSendPool.GetEntriesCount(), INODE_REJECTED, error);
                    return;
                }
            } else {
                LogPrintf("dsi -- missing input tx! %s\n", tx.ToString().c_str());
                error = _("Missing input transaction information.");
                pfrom->PushMessage("dssu", anonSendPool.sessionID, anonSendPool.GetState(), anonSendPool.GetEntriesCount(), INODE_REJECTED, error);
                return;
            }

            //if(!AcceptableInputs(mempool, state, tx)){
            bool* pfMissingInputs;
	    if(!AcceptableInputs(mempool, tx, false, pfMissingInputs)){
                LogPrintf("dsi -- transaction not valid! \n");
                error = _("Transaction not valid.");
                pfrom->PushMessage("dssu", anonSendPool.sessionID, anonSendPool.GetState(), anonSendPool.GetEntriesCount(), INODE_REJECTED, error);
                return;
            }
        }

        if(anonSendPool.AddEntry(in, nAmount, txCollateral, out, error)){
            pfrom->PushMessage("dssu", anonSendPool.sessionID, anonSendPool.GetState(), anonSendPool.GetEntriesCount(), INODE_ACCEPTED, error);
            anonSendPool.Check();

            RelayAnonSendStatus(anonSendPool.sessionID, anonSendPool.GetState(), anonSendPool.GetEntriesCount(), INODE_RESET);
        } else {
            pfrom->PushMessage("dssu", anonSendPool.sessionID, anonSendPool.GetState(), anonSendPool.GetEntriesCount(), INODE_REJECTED, error);
        }
    }

    else if (strCommand == "dssub") { //AnonSend Subscribe To
        if (pfrom->nVersion < anonSendPool.MIN_PEER_PROTO_VERSION) {
            return;
        }

        if(!fINode) return;

        std::string error = "";
        pfrom->PushMessage("dssu", anonSendPool.sessionID, anonSendPool.GetState(), anonSendPool.GetEntriesCount(), INODE_RESET, error);
        return;
    }

    else if (strCommand == "dssu") { //AnonSend status update

        if (pfrom->nVersion < anonSendPool.MIN_PEER_PROTO_VERSION) {
            return;
        }

        if((CNetAddr)anonSendPool.submittedToInode != (CNetAddr)pfrom->addr){
            //LogPrintf("dssu - message doesn't match current inode - %s != %s\n", anonSendPool.submittedToInode.ToString().c_str(), pfrom->addr.ToString().c_str());
            return;
        }

        int sessionID;
        int state;
        int entriesCount;
        int accepted;
        std::string error;
        vRecv >> sessionID >> state >> entriesCount >> accepted >> error;

        if(fDebug) LogPrintf("dssu - state: %i entriesCount: %i accepted: %i error: %s \n", state, entriesCount, accepted, error.c_str());

        if((accepted != 1 && accepted != 0) && anonSendPool.sessionID != sessionID){
            LogPrintf("dssu - message doesn't match current anonsend session %d %d\n", anonSendPool.sessionID, sessionID);
            return;
        }

        anonSendPool.StatusUpdate(state, entriesCount, accepted, error, sessionID);

    }

    else if (strCommand == "dss") { //AnonSend Sign Final Tx
        if (pfrom->nVersion < anonSendPool.MIN_PEER_PROTO_VERSION) {
            return;
        }

        vector<CTxIn> sigs;
        vRecv >> sigs;

        bool success = false;
        int count = 0;

        LogPrintf(" -- sigs count %d %d\n", (int)sigs.size(), count);

        BOOST_FOREACH(const CTxIn item, sigs)
        {
            if(anonSendPool.AddScriptSig(item)) success = true;
            if(fDebug) LogPrintf(" -- sigs count %d %d\n", (int)sigs.size(), count);
            count++;
        }

        if(success){
            anonSendPool.Check();
            RelayAnonSendStatus(anonSendPool.sessionID, anonSendPool.GetState(), anonSendPool.GetEntriesCount(), INODE_RESET);
        }
    }

}

int randomizeList (int i) { return std::rand()%i;}

// Recursively determine the rounds of a given input (How deep is the anonsend chain for a given input)
int GetInputAnonsendRounds(CTxIn in, int rounds)
{
    if(rounds >= 17) return rounds;

    std::string padding = "";
    padding.insert(0, ((rounds+1)*5)+3, ' ');

    CWalletTx tx;
    if(pwalletMain->GetTransaction(in.prevout.hash,tx))
    {
        // bounds check
        if(in.prevout.n >= tx.vout.size()) return -4;

        if(tx.vout[in.prevout.n].nValue == ANONSEND_FEE) return -3;

        //make sure the final output is non-denominate
        if(rounds == 0 && !pwalletMain->IsDenominatedAmount(tx.vout[in.prevout.n].nValue)) return -2; //NOT DENOM

        bool found = false;
        BOOST_FOREACH(CTxOut out, tx.vout)
        {
            found = pwalletMain->IsDenominatedAmount(out.nValue);
            if(found) break; // no need to loop more
        }
        if(!found) return rounds - 1; //NOT FOUND, "-1" because of the pre-mixing creation of denominated amounts

        // find my vin and look that up
        BOOST_FOREACH(CTxIn in2, tx.vin)
        {
            if(pwalletMain->IsMine(in2))
            {
                //LogPrintf("rounds :: %s %s %d NEXT\n", padding.c_str(), in.ToString().c_str(), rounds);
                int n = GetInputAnonsendRounds(in2, rounds+1);
                if(n != -3) return n;
            }
        }
    }

    return rounds-1;
}

void CAnonSendPool::Reset(){
    cachedLastSuccess = 0;
    vecInodesUsed.clear();
    UnlockCoins();
    SetNull();
}

void CAnonSendPool::SetNull(bool clearEverything){
    finalTransaction.vin.clear();
    finalTransaction.vout.clear();

    entries.clear();

    state = POOL_STATUS_ACCEPTING_ENTRIES;

    lastTimeChanged = GetTimeMillis();

    entriesCount = 0;
    lastEntryAccepted = 0;
    countEntriesAccepted = 0;
    lastNewBlock = 0;

    sessionUsers = 0;
    sessionDenom = 0;
    sessionFoundInode = false;
    vecSessionCollateral.clear();
    txCollateral = CTransaction();

    if(clearEverything){
        myEntries.clear();

        if(fINode){
            sessionID = 1 + (rand() % 999999);
        } else {
            sessionID = 0;
        }
    }

    // -- seed random number generator (used for ordering output lists)
    unsigned int seed = 0;
    GetRandBytes((unsigned char*)&seed, sizeof(seed));
    std::srand(seed);
}

bool CAnonSendPool::SetCollateralAddress(std::string strAddress){
    CBitcoinAddress address;
    if (!address.SetString(strAddress))
    {
        LogPrintf("CAnonSendPool::SetCollateralAddress - Invalid AnonSend collateral address\n");
        return false;
    }
    collateralPubKey= GetScriptForDestination(address.Get());
    return true;
}

//
// Unlock coins after Anonsend fails or succeeds
//
void CAnonSendPool::UnlockCoins(){
    BOOST_FOREACH(CTxIn v, lockedCoins)
        pwalletMain->UnlockCoin(v.prevout);

    lockedCoins.clear();
}

//
// Check the Anonsend progress and send client updates if a inode
//
void CAnonSendPool::Check()
{
    if(fDebug) LogPrintf("CAnonSendPool::Check()\n");
    if(fDebug) LogPrintf("CAnonSendPool::Check() - entries count %lu\n", entries.size());

    // If entries is full, then move on to the next phase
    if(state == POOL_STATUS_ACCEPTING_ENTRIES && (int)entries.size() >= GetMaxPoolTransactions())
    {
        if(fDebug) LogPrintf("CAnonSendPool::Check() -- ACCEPTING OUTPUTS\n");
        UpdateState(POOL_STATUS_FINALIZE_TRANSACTION);
    }

    // create the finalized transaction for distribution to the clients
    if(state == POOL_STATUS_FINALIZE_TRANSACTION && finalTransaction.vin.empty() && finalTransaction.vout.empty()) {
        if(fDebug) LogPrintf("CAnonSendPool::Check() -- FINALIZE TRANSACTIONS\n");
        UpdateState(POOL_STATUS_SIGNING);

        if (fINode) {
            // make our new transaction
            CTransaction txNew;
            for(unsigned int i = 0; i < entries.size(); i++){
                BOOST_FOREACH(const CTxOut v, entries[i].vout)
                    txNew.vout.push_back(v);

                BOOST_FOREACH(const CAnonSendEntryVin s, entries[i].sev)
                    txNew.vin.push_back(s.vin);
            }
            // shuffle the outputs for improved anonymity
            std::random_shuffle ( txNew.vout.begin(), txNew.vout.end(), randomizeList);

            if(fDebug) LogPrintf("Transaction 1: %s\n", txNew.ToString().c_str());

            SignFinalTransaction(txNew, NULL);

            // request signatures from clients
            RelayAnonSendFinalTransaction(sessionID, txNew);
        }
    }

    // collect signatures from clients

    // If we have all of the signatures, try to compile the transaction
    if(state == POOL_STATUS_SIGNING && SignaturesComplete()) {
        if(fDebug) LogPrintf("CAnonSendPool::Check() -- SIGNING\n");
        UpdateState(POOL_STATUS_TRANSMISSION);

        CWalletTx txNew = CWalletTx(pwalletMain, finalTransaction);

        LOCK2(cs_main, pwalletMain->cs_wallet);
        {
            if (fINode) { //only the main node is inode atm
                if(fDebug) LogPrintf("Transaction 2: %s\n", txNew.ToString().c_str());

                // See if the transaction is valid
                if (!txNew.AcceptToMemoryPool(true))
                {
                    LogPrintf("CAnonSendPool::Check() - CommitTransaction : Error: Transaction not valid\n");
                    SetNull();
                    pwalletMain->Lock();

                    // not much we can do in this case
                    UpdateState(POOL_STATUS_ACCEPTING_ENTRIES);
                    RelayAnonSendCompletedTransaction(sessionID, true, "Transaction not valid, please try again");
                    return;
                }

                LogPrintf("CAnonSendPool::Check() -- IS INODE -- TRANSMITTING ANONSEND\n");

                // sign a message

                int64_t sigTime = GetAdjustedTime();
                std::string strMessage = txNew.GetHash().ToString() + boost::lexical_cast<std::string>(sigTime);
                std::string strError = "";
                std::vector<unsigned char> vchSig;
                CKey key2;
                CPubKey pubkey2;

                if(!anonSendSigner.SetKey(strINodePrivKey, strError, key2, pubkey2))
                {
                    LogPrintf("CAnonSendPool::Check() - ERROR: Invalid inodeprivkey: '%s'\n", strError.c_str());
                    return;
                }

                if(!anonSendSigner.SignMessage(strMessage, strError, vchSig, key2)) {
                    LogPrintf("CAnonSendPool::Check() - Sign message failed\n");
                    return;
                }

                if(!anonSendSigner.VerifyMessage(pubkey2, vchSig, strMessage, strError)) {
                    LogPrintf("CAnonSendPool::Check() - Verify message failed\n");
                    return;
                }

                if(!mapAnonsendBroadcastTxes.count(txNew.GetHash())){
                    CAnonsendBroadcastTx dstx;
                    dstx.tx = txNew;
                    dstx.vin = activeInode.vin;
                    dstx.vchSig = vchSig;
                    dstx.sigTime = sigTime;

                    mapAnonsendBroadcastTxes.insert(make_pair(txNew.GetHash(), dstx));
                }

                // Broadcast the transaction to the network
                txNew.fTimeReceivedIsTxTime = true;
                txNew.RelayWalletTransaction();

                // Tell the clients it was successful
                RelayAnonSendCompletedTransaction(sessionID, false, _("Transaction created successfully."));

                // Randomly charge clients
                ChargeRandomFees();
            }
        }
    }

    // move on to next phase, allow 3 seconds incase the inode wants to send us anything else
    if((state == POOL_STATUS_TRANSMISSION && fINode) || (state == POOL_STATUS_SIGNING && completedTransaction) ) {
        if(fDebug) LogPrintf("CAnonSendPool::Check() -- COMPLETED -- RESETTING \n");
        SetNull(true);
        UnlockCoins();
        if(fINode) RelayAnonSendStatus(anonSendPool.sessionID, anonSendPool.GetState(), anonSendPool.GetEntriesCount(), INODE_RESET);
        pwalletMain->Lock();
    }

    // reset if we're here for 10 seconds
    if((state == POOL_STATUS_ERROR || state == POOL_STATUS_SUCCESS) && GetTimeMillis()-lastTimeChanged >= 10000) {
        if(fDebug) LogPrintf("CAnonSendPool::Check() -- RESETTING MESSAGE \n");
        SetNull(true);
        if(fINode) RelayAnonSendStatus(anonSendPool.sessionID, anonSendPool.GetState(), anonSendPool.GetEntriesCount(), INODE_RESET);
        UnlockCoins();
    }
}

//
// Charge clients a fee if they're abusive
//
// Why bother? Anonsend uses collateral to ensure abuse to the process is kept to a minimum.
// The submission and signing stages in anonsend are completely separate. In the cases where
// a client submits a transaction then refused to sign, there must be a cost. Otherwise they
// would be able to do this over and over again and bring the mixing to a hault.
//
// How does this work? Messages to inodes come in via "dsi", these require a valid collateral
// transaction for the client to be able to enter the pool. This transaction is kept by the inode
// until the transaction is either complete or fails.
//
void CAnonSendPool::ChargeFees(){
    if(fINode) {
        //we don't need to charge collateral for every offence.
        int offences = 0;
        int r = rand()%100;
        if(r > 33) return;

        if(state == POOL_STATUS_ACCEPTING_ENTRIES){
            BOOST_FOREACH(const CTransaction& txCollateral, vecSessionCollateral) {
                bool found = false;
                BOOST_FOREACH(const CAnonSendEntry& v, entries) {
                    if(v.collateral == txCollateral) {
                        found = true;
                    }
                }

                // This queue entry didn't send us the promised transaction
                if(!found){
                    LogPrintf("CAnonSendPool::ChargeFees -- found uncooperative node (didn't send transaction). Found offence.\n");
                    offences++;
                }
            }
        }

        if(state == POOL_STATUS_SIGNING) {
            // who didn't sign?
            BOOST_FOREACH(const CAnonSendEntry v, entries) {
                BOOST_FOREACH(const CAnonSendEntryVin s, v.sev) {
                    if(!s.isSigSet){
                        LogPrintf("CAnonSendPool::ChargeFees -- found uncooperative node (didn't sign). Found offence\n");
                        offences++;
                    }
                }
            }
        }

        r = rand()%100;
        int target = 0;

        //mostly offending?
        if(offences >= POOL_MAX_TRANSACTIONS-1 && r > 33) return;

        //everyone is an offender? That's not right
        if(offences >= POOL_MAX_TRANSACTIONS) return;

        //charge one of the offenders randomly
        if(offences > 1) target = 50;

        //pick random client to charge
        r = rand()%100;

        if(state == POOL_STATUS_ACCEPTING_ENTRIES){
            BOOST_FOREACH(const CTransaction& txCollateral, vecSessionCollateral) {
                bool found = false;
                BOOST_FOREACH(const CAnonSendEntry& v, entries) {
                    if(v.collateral == txCollateral) {
                        found = true;
                    }
                }

                // This queue entry didn't send us the promised transaction
                if(!found && r > target){
                    LogPrintf("CAnonSendPool::ChargeFees -- found uncooperative node (didn't send transaction). charging fees.\n");

                    CWalletTx wtxCollateral = CWalletTx(pwalletMain, txCollateral);

                    // Broadcast
                    if (!wtxCollateral.AcceptToMemoryPool(true))
                    {
                        // This must not fail. The transaction has already been signed and recorded.
                        LogPrintf("CAnonSendPool::ChargeFees() : Error: Transaction not valid");
                    }
                    wtxCollateral.RelayWalletTransaction();
                    return;
                }
            }
        }

        if(state == POOL_STATUS_SIGNING) {
            // who didn't sign?
            BOOST_FOREACH(const CAnonSendEntry v, entries) {
                BOOST_FOREACH(const CAnonSendEntryVin s, v.sev) {
                    if(!s.isSigSet && r > target){
                        LogPrintf("CAnonSendPool::ChargeFees -- found uncooperative node (didn't sign). charging fees.\n");

                        CWalletTx wtxCollateral = CWalletTx(pwalletMain, v.collateral);

                        // Broadcast
                        if (!wtxCollateral.AcceptToMemoryPool(true))
                        {
                            // This must not fail. The transaction has already been signed and recorded.
                            LogPrintf("CAnonSendPool::ChargeFees() : Error: Transaction not valid");
                        }
                        wtxCollateral.RelayWalletTransaction();
                        return;
                    }
                }
            }
        }
    }
}

// charge the collateral randomly
//  - Anonsend is completely free, to pay miners we randomly pay the collateral of users.
void CAnonSendPool::ChargeRandomFees(){
    if(fINode) {
        int i = 0;

        BOOST_FOREACH(const CTransaction& txCollateral, vecSessionCollateral) {
            int r = rand()%1000;

            /*
                Collateral Fee Charges:

                Being that AnonSend has "no fees" we need to have some kind of cost associated
                with using it to stop abuse. Otherwise it could serve as an attack vector and
                allow endless transaction that would bloat NavCoin and make it unusable. To
                stop these kinds of attacks 1 in 50 successful transactions are charged. This
                adds up to a cost of 0.002NAV per transaction on average.
            */
            if(r <= 20)
            {
                LogPrintf("CAnonSendPool::ChargeRandomFees -- charging random fees. %u\n", i);

                CWalletTx wtxCollateral = CWalletTx(pwalletMain, txCollateral);

                // Broadcast
                if (!wtxCollateral.AcceptToMemoryPool(true))
                {
                    // This must not fail. The transaction has already been signed and recorded.
                    LogPrintf("CAnonSendPool::ChargeRandomFees() : Error: Transaction not valid");
                }
                wtxCollateral.RelayWalletTransaction();
            }
        }
    }
}

//
// Check for various timeouts (queue objects, anonsend, etc)
//
void CAnonSendPool::CheckTimeout(){
    if(!fEnableAnonsend && !fINode) return;

    // catching hanging sessions
    if(!fINode) {
        if(state == POOL_STATUS_TRANSMISSION) {
            if(fDebug) LogPrintf("CAnonSendPool::CheckTimeout() -- Session complete -- Running Check()\n");
            Check();
        }
    }

    // check anonsend queue objects for timeouts
    int c = 0;
    vector<CAnonsendQueue>::iterator it;
    for(it=vecAnonsendQueue.begin();it<vecAnonsendQueue.end();it++){
        if((*it).IsExpired()){
            if(fDebug) LogPrintf("CAnonSendPool::CheckTimeout() : Removing expired queue entry - %d\n", c);
            vecAnonsendQueue.erase(it);
            break;
        }
        c++;
    }

    /* Check to see if we're ready for submissions from clients */
    if(state == POOL_STATUS_QUEUE && sessionUsers == GetMaxPoolTransactions()) {
        CAnonsendQueue dsq;
        dsq.nDenom = sessionDenom;
        dsq.vin = activeInode.vin;
        dsq.time = GetTime();
        dsq.ready = true;
        dsq.Sign();
        dsq.Relay();

        UpdateState(POOL_STATUS_ACCEPTING_ENTRIES);
    }

    int addLagTime = 0;
    if(!fINode) addLagTime = 5000; //if we're the client, give the server a few extra seconds before resetting.

    if(state == POOL_STATUS_ACCEPTING_ENTRIES || state == POOL_STATUS_QUEUE){
        c = 0;

        // if it's a inode, the entries are stored in "entries", otherwise they're stored in myEntries
        std::vector<CAnonSendEntry> *vec = &myEntries;
        if(fINode) vec = &entries;

        // check for a timeout and reset if needed
        vector<CAnonSendEntry>::iterator it2;
        for(it2=vec->begin();it2<vec->end();it2++){
            if((*it2).IsExpired()){
                if(fDebug) LogPrintf("CAnonSendPool::CheckTimeout() : Removing expired entry - %d\n", c);
                vec->erase(it2);
                if(entries.size() == 0 && myEntries.size() == 0){
                    SetNull(true);
                    UnlockCoins();
                }
                if(fINode){
                    RelayAnonSendStatus(anonSendPool.sessionID, anonSendPool.GetState(), anonSendPool.GetEntriesCount(), INODE_RESET);
                }
                break;
            }
            c++;
        }

        if(GetTimeMillis()-lastTimeChanged >= (ANONSEND_QUEUE_TIMEOUT*1000)+addLagTime){
            lastTimeChanged = GetTimeMillis();

            ChargeFees();
            // reset session information for the queue query stage (before entering a inode, clients will send a queue request to make sure they're compatible denomination wise)
            sessionUsers = 0;
            sessionDenom = 0;
            sessionFoundInode = false;
            vecSessionCollateral.clear();

            UpdateState(POOL_STATUS_ACCEPTING_ENTRIES);
        }
    } else if(GetTimeMillis()-lastTimeChanged >= (ANONSEND_QUEUE_TIMEOUT*1000)+addLagTime){
        if(fDebug) LogPrintf("CAnonSendPool::CheckTimeout() -- Session timed out (30s) -- resetting\n");
        SetNull();
        UnlockCoins();

        UpdateState(POOL_STATUS_ERROR);
        lastMessage = _("Session timed out (30 seconds), please resubmit.");
    }

    if(state == POOL_STATUS_SIGNING && GetTimeMillis()-lastTimeChanged >= (ANONSEND_SIGNING_TIMEOUT*1000)+addLagTime ) {
        if(fDebug) LogPrintf("CAnonSendPool::CheckTimeout() -- Session timed out -- restting\n");
        ChargeFees();
        SetNull();
        UnlockCoins();
        //add my transactions to the new session

        UpdateState(POOL_STATUS_ERROR);
        lastMessage = _("Signing timed out, please resubmit.");
    }
}

// check to see if the signature is valid
bool CAnonSendPool::SignatureValid(const CScript& newSig, const CTxIn& newVin){
    CTransaction txNew;
    txNew.vin.clear();
    txNew.vout.clear();

    int found = -1;
    CScript sigPubKey = CScript();
    unsigned int i = 0;

    BOOST_FOREACH(CAnonSendEntry e, entries) {
        BOOST_FOREACH(const CTxOut out, e.vout)
            txNew.vout.push_back(out);

        BOOST_FOREACH(const CAnonSendEntryVin s, e.sev){
            txNew.vin.push_back(s.vin);

            if(s.vin == newVin){
                found = i;
                sigPubKey = s.vin.prevPubKey;
            }
            i++;
        }
    }

    if(found >= 0){ //might have to do this one input at a time?
        int n = found;
        txNew.vin[n].scriptSig = newSig;
        if(fDebug) LogPrintf("CAnonSendPool::SignatureValid() - Sign with sig %s\n", newSig.ToString().substr(0,24).c_str());
        if (!VerifyScript(txNew.vin[n].scriptSig, sigPubKey, SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_STRICTENC, SignatureChecker(txNew, i))){
            if(fDebug) LogPrintf("CAnonSendPool::SignatureValid() - Signing - Error signing input %u\n", n);
            return false;
        }
    }

    if(fDebug) LogPrintf("CAnonSendPool::SignatureValid() - Signing - Succesfully signed input\n");
    return true;
}

// check to make sure the collateral provided by the client is valid
bool CAnonSendPool::IsCollateralValid(const CTransaction& txCollateral){
    if(txCollateral.vout.size() < 1) return false;
    if(txCollateral.nLockTime != 0) return false;

    int64_t nValueIn = 0;
    int64_t nValueOut = 0;
    bool missingTx = false;

    BOOST_FOREACH(const CTxOut o, txCollateral.vout){
        nValueOut += o.nValue;

        if(!o.scriptPubKey.IsNormalPaymentScript()){
            LogPrintf ("CAnonSendPool::IsCollateralValid - Invalid Script %s\n", txCollateral.ToString().c_str());
            return false;
        }
    }

    BOOST_FOREACH(const CTxIn i, txCollateral.vin){
        CTransaction tx2;
        uint256 hash;
        //if(GetTransaction(i.prevout.hash, tx2, hash, true)){
	if(GetTransaction(i.prevout.hash, tx2, hash)){
            if(tx2.vout.size() > i.prevout.n) {
                nValueIn += tx2.vout[i.prevout.n].nValue;
            }
        } else{
            missingTx = true;
        }
    }

    if(missingTx){
        if(fDebug) LogPrintf ("CAnonSendPool::IsCollateralValid - Unknown inputs in collateral transaction - %s\n", txCollateral.ToString().c_str());
        return false;
    }

    //collateral transactions are required to pay out ANONSEND_COLLATERAL as a fee to the miners
    if(nValueIn-nValueOut < ANONSEND_COLLATERAL) {
        if(fDebug) LogPrintf ("CAnonSendPool::IsCollateralValid - did not include enough fees in transaction %d\n%s\n", nValueOut-nValueIn, txCollateral.ToString().c_str());
        return false;
    }

    if(fDebug) LogPrintf("CAnonSendPool::IsCollateralValid %s\n", txCollateral.ToString().c_str());

    CValidationState state;
    //if(!AcceptableInputs(mempool, state, txCollateral)){
    bool* pfMissingInputs = NULL;
    if(!AcceptableInputs(mempool, txCollateral, false, pfMissingInputs)){
        if(fDebug) LogPrintf ("CAnonSendPool::IsCollateralValid - didn't pass IsAcceptable\n");
        return false;
    }

    return true;
}


//
// Add a clients transaction to the pool
//
bool CAnonSendPool::AddEntry(const std::vector<CTxIn>& newInput, const int64_t& nAmount, const CTransaction& txCollateral, const std::vector<CTxOut>& newOutput, std::string& error){
    if (!fINode) return false;

    BOOST_FOREACH(CTxIn in, newInput) {
        if (in.prevout.IsNull() || nAmount < 0) {
            if(fDebug) LogPrintf ("CAnonSendPool::AddEntry - input not valid!\n");
            error = _("Input is not valid.");
            sessionUsers--;
            return false;
        }
    }

    if (!IsCollateralValid(txCollateral)){
        if(fDebug) LogPrintf ("CAnonSendPool::AddEntry - collateral not valid!\n");
        error = _("Collateral is not valid.");
        sessionUsers--;
        return false;
    }

    if((int)entries.size() >= GetMaxPoolTransactions()){
        if(fDebug) LogPrintf ("CAnonSendPool::AddEntry - entries is full!\n");
        error = _("Entries are full.");
        sessionUsers--;
        return false;
    }

    BOOST_FOREACH(CTxIn in, newInput) {
        if(fDebug) LogPrintf("looking for vin -- %s\n", in.ToString().c_str());
        BOOST_FOREACH(const CAnonSendEntry v, entries) {
            BOOST_FOREACH(const CAnonSendEntryVin s, v.sev){
                if(s.vin == in) {
                    if(fDebug) LogPrintf ("CAnonSendPool::AddEntry - found in vin\n");
                    error = _("Already have that input.");
                    sessionUsers--;
                    return false;
                }
            }
        }
    }

    if(state == POOL_STATUS_ACCEPTING_ENTRIES) {
        CAnonSendEntry v;
        v.Add(newInput, nAmount, txCollateral, newOutput);
        entries.push_back(v);

        if(fDebug) LogPrintf("CAnonSendPool::AddEntry -- adding %s\n", newInput[0].ToString().c_str());
        error = "";

        return true;
    }

    if(fDebug) LogPrintf ("CAnonSendPool::AddEntry - can't accept new entry, wrong state!\n");
    error = _("Wrong state.");
    sessionUsers--;
    return false;
}

bool CAnonSendPool::AddScriptSig(const CTxIn newVin){
    if(fDebug) LogPrintf("CAnonSendPool::AddScriptSig -- new sig  %s\n", newVin.scriptSig.ToString().substr(0,24).c_str());

    BOOST_FOREACH(const CAnonSendEntry v, entries) {
        BOOST_FOREACH(const CAnonSendEntryVin s, v.sev){
            if(s.vin.scriptSig == newVin.scriptSig) {
                LogPrintf("CAnonSendPool::AddScriptSig - already exists \n");
                return false;
            }
        }
    }

    if(!SignatureValid(newVin.scriptSig, newVin)){
        if(fDebug) LogPrintf("CAnonSendPool::AddScriptSig - Invalid Sig\n");
        return false;
    }

    if(fDebug) LogPrintf("CAnonSendPool::AddScriptSig -- sig %s\n", newVin.ToString().c_str());

    if(state == POOL_STATUS_SIGNING) {
        BOOST_FOREACH(CTxIn& vin, finalTransaction.vin){
            if(newVin.prevout == vin.prevout && vin.nSequence == newVin.nSequence){
                vin.scriptSig = newVin.scriptSig;
                vin.prevPubKey = newVin.prevPubKey;
                if(fDebug) LogPrintf("CAnonSendPool::AddScriptSig -- adding to finalTransaction  %s\n", newVin.scriptSig.ToString().substr(0,24).c_str());
            }
        }
        for(unsigned int i = 0; i < entries.size(); i++){
            if(entries[i].AddSig(newVin)){
                if(fDebug) LogPrintf("CAnonSendPool::AddScriptSig -- adding  %s\n", newVin.scriptSig.ToString().substr(0,24).c_str());
                return true;
            }
        }
    }

    LogPrintf("CAnonSendPool::AddScriptSig -- Couldn't set sig!\n" );
    return false;
}

// check to make sure everything is signed
bool CAnonSendPool::SignaturesComplete(){
    BOOST_FOREACH(const CAnonSendEntry v, entries) {
        BOOST_FOREACH(const CAnonSendEntryVin s, v.sev){
            if(!s.isSigSet) return false;
        }
    }
    return true;
}

//
// Execute a anonsend denomination via a inode.
// This is only ran from clients
//
void CAnonSendPool::SendAnonsendDenominate(std::vector<CTxIn>& vin, std::vector<CTxOut>& vout, int64_t amount){
    if(anonSendPool.txCollateral == CTransaction()){
        LogPrintf ("CAnonsendPool:SendAnonsendDenominate() - Anonsend collateral not set");
        return;
    }

    // lock the funds we're going to use
    BOOST_FOREACH(CTxIn in, txCollateral.vin)
        lockedCoins.push_back(in);

    BOOST_FOREACH(CTxIn in, vin)
        lockedCoins.push_back(in);

    //BOOST_FOREACH(CTxOut o, vout)
    //    LogPrintf(" vout - %s\n", o.ToString().c_str());


    // we should already be connected to a inode
    if(!sessionFoundInode){
        LogPrintf("CAnonSendPool::SendAnonsendDenominate() - No inode has been selected yet.\n");
        UnlockCoins();
        SetNull(true);
        return;
    }

    if (!CheckDiskSpace())
        return;

    if(fINode) {
        LogPrintf("CAnonSendPool::SendAnonsendDenominate() - AnonSend from a inode is not supported currently.\n");
        return;
    }

    UpdateState(POOL_STATUS_ACCEPTING_ENTRIES);

    LogPrintf("CAnonSendPool::SendAnonsendDenominate() - Added transaction to pool.\n");

    ClearLastMessage();

    //check it against the memory pool to make sure it's valid
    {
        int64_t nValueOut = 0;

        CValidationState state;
        CTransaction tx;

        BOOST_FOREACH(const CTxOut o, vout){
            nValueOut += o.nValue;
            tx.vout.push_back(o);
        }

        BOOST_FOREACH(const CTxIn i, vin){
            tx.vin.push_back(i);

            if(fDebug) LogPrintf("dsi -- tx in %s\n", i.ToString().c_str());
        }

        //if(!AcceptableInputs(mempool, state, tx)){
	bool* pfMissingInputs;
	if(!AcceptableInputs(mempool, tx, false, pfMissingInputs)){
            LogPrintf("dsi -- transaction not valid! %s \n", tx.ToString().c_str());
            return;
        }
    }

    // store our entry for later use
    CAnonSendEntry e;
    e.Add(vin, amount, txCollateral, vout);
    myEntries.push_back(e);

    // relay our entry to the inode
    RelayAnonSendIn(vin, amount, txCollateral, vout);
    Check();
}

// Incoming message from inode updating the progress of anonsend
//    newAccepted:  -1 mean's it'n not a "transaction accepted/not accepted" message, just a standard update
//                  0 means transaction was not accepted
//                  1 means transaction was accepted

bool CAnonSendPool::StatusUpdate(int newState, int newEntriesCount, int newAccepted, std::string& error, int newSessionID){
    if(fINode) return false;
    if(state == POOL_STATUS_ERROR || state == POOL_STATUS_SUCCESS) return false;

    UpdateState(newState);
    entriesCount = newEntriesCount;

    if(error.size() > 0) strAutoDenomResult = _("Inode:") + " " + error;

    if(newAccepted != -1) {
        lastEntryAccepted = newAccepted;
        countEntriesAccepted += newAccepted;
        if(newAccepted == 0){
            UpdateState(POOL_STATUS_ERROR);
            lastMessage = error;
        }

        if(newAccepted == 1) {
            sessionID = newSessionID;
            LogPrintf("CAnonSendPool::StatusUpdate - set sessionID to %d\n", sessionID);
            sessionFoundInode = true;
        }
    }

    if(newState == POOL_STATUS_ACCEPTING_ENTRIES){
        if(newAccepted == 1){
            LogPrintf("CAnonSendPool::StatusUpdate - entry accepted! \n");
            sessionFoundInode = true;
            //wait for other users. Inode will report when ready
            UpdateState(POOL_STATUS_QUEUE);
        } else if (newAccepted == 0 && sessionID == 0 && !sessionFoundInode) {
            LogPrintf("CAnonSendPool::StatusUpdate - entry not accepted by inode \n");
            UnlockCoins();
            UpdateState(POOL_STATUS_ACCEPTING_ENTRIES);
            DoAutomaticDenominating(); //try another inode
        }
        if(sessionFoundInode) return true;
    }

    return true;
}

//
// After we receive the finalized transaction from the inode, we must
// check it to make sure it's what we want, then sign it if we agree.
// If we refuse to sign, it's possible we'll be charged collateral
//
bool CAnonSendPool::SignFinalTransaction(CTransaction& finalTransactionNew, CNode* node){
    if(fDebug) LogPrintf("CAnonSendPool::AddFinalTransaction - Got Finalized Transaction\n");

    if(!finalTransaction.vin.empty()){
        LogPrintf("CAnonSendPool::AddFinalTransaction - Rejected Final Transaction!\n");
        return false;
    }

    finalTransaction = finalTransactionNew;
    LogPrintf("CAnonSendPool::SignFinalTransaction %s\n", finalTransaction.ToString().c_str());

    vector<CTxIn> sigs;

    //make sure my inputs/outputs are present, otherwise refuse to sign
    BOOST_FOREACH(const CAnonSendEntry e, myEntries) {
        BOOST_FOREACH(const CAnonSendEntryVin s, e.sev) {
            /* Sign my transaction and all outputs */
            int mine = -1;
            CScript prevPubKey = CScript();
            CTxIn vin = CTxIn();

            for(unsigned int i = 0; i < finalTransaction.vin.size(); i++){
                if(finalTransaction.vin[i] == s.vin){
                    mine = i;
                    prevPubKey = s.vin.prevPubKey;
                    vin = s.vin;
                }
            }

            if(mine >= 0){ //might have to do this one input at a time?
                int foundOutputs = 0;
                int64_t nValue1 = 0;
                int64_t nValue2 = 0;

                for(unsigned int i = 0; i < finalTransaction.vout.size(); i++){
                    BOOST_FOREACH(const CTxOut o, e.vout) {
                        if(finalTransaction.vout[i] == o){
                            foundOutputs++;
                            nValue1 += finalTransaction.vout[i].nValue;
                        }
                    }
                }

                BOOST_FOREACH(const CTxOut o, e.vout)
                    nValue2 += o.nValue;

                int targetOuputs = e.vout.size();
                if(foundOutputs < targetOuputs || nValue1 != nValue2) {
                    // in this case, something went wrong and we'll refuse to sign. It's possible we'll be charged collateral. But that's
                    // better then signing if the transaction doesn't look like what we wanted.
                    LogPrintf("CAnonSendPool::Sign - My entries are not correct! Refusing to sign. %d entries %d target. \n", foundOutputs, targetOuputs);
                    return false;
                }
				
                if(fDebug) LogPrintf("CAnonSendPool::Sign - Signing my input %i\n", mine);
                if(!SignSignature(*pwalletMain, prevPubKey, finalTransaction, mine, int(SIGHASH_ALL|SIGHASH_ANYONECANPAY))) { // changes scriptSig
                    if(fDebug) LogPrintf("CAnonSendPool::Sign - Unable to sign my own transaction! \n");
                    // not sure what to do here, it will timeout...?
                }

                sigs.push_back(finalTransaction.vin[mine]);
                if(fDebug) LogPrintf(" -- dss %d %d %s\n", mine, (int)sigs.size(), finalTransaction.vin[mine].scriptSig.ToString().c_str());
            }

        }

        if(fDebug) LogPrintf("CAnonSendPool::Sign - txNew:\n%s", finalTransaction.ToString().c_str());
    }

    // push all of our signatures to the inode
    if(sigs.size() > 0 && node != NULL)
        node->PushMessage("dss", sigs);

    return true;
}

void CAnonSendPool::NewBlock()
{
    if(fDebug) LogPrintf("CAnonSendPool::NewBlock \n");

    //we we're processing lots of blocks, we'll just leave
    if(GetTime() - lastNewBlock < 10) return;
    lastNewBlock = GetTime();

    anonSendPool.CheckTimeout();

    if(!fEnableAnonsend) return;

    if(!fINode){
        //denominate all non-denominated inputs every 25 minutes.
        if(pindexBest->nHeight % 10 == 0) UnlockCoins();
        ProcessInodeConnections();
    }
}

// Anonsend transaction was completed (failed or successed)
void CAnonSendPool::CompletedTransaction(bool error, std::string lastMessageNew)
{
    if(fINode) return;

    if(error){
        LogPrintf("CompletedTransaction -- error \n");
        UpdateState(POOL_STATUS_ERROR);
    } else {
        LogPrintf("CompletedTransaction -- success \n");
        UpdateState(POOL_STATUS_SUCCESS);

        myEntries.clear();

        // To avoid race conditions, we'll only let DS run once per block
        cachedLastSuccess = pindexBest->nHeight;
    }
    lastMessage = lastMessageNew;

    completedTransaction = true;
    Check();
    UnlockCoins();
}

void CAnonSendPool::ClearLastMessage()
{
    lastMessage = "";
}

//
// Passively run Anonsend in the background to anonymize funds based on the given configuration.
//
// This does NOT run by default for daemons, only for QT.
//
bool CAnonSendPool::DoAutomaticDenominating(bool fDryRun, bool ready)
{
    LOCK(cs_anonsend);

    if(IsInitialBlockDownload()) return false;

    if(fINode) return false;
    if(state == POOL_STATUS_ERROR || state == POOL_STATUS_SUCCESS) return false;

    if(pindexBest->nHeight - cachedLastSuccess < minBlockSpacing) {
        LogPrintf("CAnonSendPool::DoAutomaticDenominating - Last successful anonsend action was too recent\n");
        strAutoDenomResult = _("Last successful anonsend action was too recent.");
        return false;
    }
    if(!fEnableAnonsend) {
        if(fDebug) LogPrintf("CAnonSendPool::DoAutomaticDenominating - Anonsend is disabled\n");
        strAutoDenomResult = _("Anonsend is disabled.");
        return false;
    }

    if (!fDryRun && pwalletMain->IsLocked()){
        strAutoDenomResult = _("Wallet is locked.");
        return false;
    }

    if(anonSendPool.GetState() != POOL_STATUS_ERROR && anonSendPool.GetState() != POOL_STATUS_SUCCESS){
        if(anonSendPool.GetMyTransactionCount() > 0){
            return true;
        }
    }

    if(vecInodes.size() == 0){
        if(fDebug) LogPrintf("CAnonSendPool::DoAutomaticDenominating - No inodes detected\n");
        strAutoDenomResult = _("No inodes detected.");
        return false;
    }

    // ** find the coins we'll use
    std::vector<CTxIn> vCoins;
    std::vector<COutput> vCoins2;
    int64_t nValueMin = CENT;
    int64_t nValueIn = 0;

    // should not be less than fees in ANONSEND_FEE + few (lets say 5) smallest denoms
    int64_t nLowestDenom = ANONSEND_FEE + anonSendDenominations[anonSendDenominations.size() - 1]*5;

    // if there are no DS collateral inputs yet
    if(!pwalletMain->HasCollateralInputs())
        // should have some additional amount for them
        nLowestDenom += (ANONSEND_COLLATERAL*4)+ANONSEND_FEE*2;

    int64_t nBalanceNeedsAnonymized = nAnonymizeNavCoinAmount*COIN - pwalletMain->GetAnonymizedBalance();

    // if balanceNeedsAnonymized is more than pool max, take the pool max
    if(nBalanceNeedsAnonymized > ANONSEND_POOL_MAX) nBalanceNeedsAnonymized = ANONSEND_POOL_MAX;

    // if balanceNeedsAnonymized is more than non-anonymized, take non-anonymized
    int64_t nBalanceNotYetAnonymized = pwalletMain->GetBalance() - pwalletMain->GetAnonymizedBalance();
    if(nBalanceNeedsAnonymized > nBalanceNotYetAnonymized) nBalanceNeedsAnonymized = nBalanceNotYetAnonymized;

    if(nBalanceNeedsAnonymized < nLowestDenom)
    {
//        if(nBalanceNeedsAnonymized > nValueMin)
//            nBalanceNeedsAnonymized = nLowestDenom;
//        else
//        {
            LogPrintf("DoAutomaticDenominating : No funds detected in need of denominating \n");
            strAutoDenomResult = _("No funds detected in need of denominating.");
            return false;
//        }
    }

    if (fDebug) LogPrintf("DoAutomaticDenominating : nLowestDenom=%d, nBalanceNeedsAnonymized=%d\n", nLowestDenom, nBalanceNeedsAnonymized);

    // select coins that should be given to the pool
    if (!pwalletMain->SelectCoinsAnon(nValueMin, nBalanceNeedsAnonymized, vCoins, nValueIn, 0, nAnonsendRounds))
    {
        nValueIn = 0;
        vCoins.clear();

        if (pwalletMain->SelectCoinsAnon(nValueMin, 9999999*COIN, vCoins, nValueIn, -2, 0))
        {
            if(!fDryRun) return CreateDenominated(nBalanceNeedsAnonymized);
            return true;
        } else {
            LogPrintf("DoAutomaticDenominating : Can't denominate - no compatible inputs left\n");
            strAutoDenomResult = _("Can't denominate: no compatible inputs left.");
            return false;
        }

    }

    //check to see if we have the collateral sized inputs, it requires these
    if(!pwalletMain->HasCollateralInputs()){
        if(!fDryRun) MakeCollateralAmounts();
        return true;
    }

    if(fDryRun) return true;

    // initial phase, find a inode
    if(!sessionFoundInode){
        int nUseQueue = rand()%100;

        sessionTotalValue = pwalletMain->GetTotalValue(vCoins);

        //randomize the amounts we mix
        if(sessionTotalValue > nBalanceNeedsAnonymized) sessionTotalValue = nBalanceNeedsAnonymized;

        double fNavCoinSubmitted = (sessionTotalValue / CENT);
        LogPrintf("Submitting Anonsend for %f NAV CENT - sessionTotalValue %d\n", fNavCoinSubmitted, sessionTotalValue);

        if(pwalletMain->GetDenominatedBalance(true, true) > 0){ //get denominated unconfirmed inputs
            LogPrintf("DoAutomaticDenominating -- Found unconfirmed denominated outputs, will wait till they confirm to continue.\n");
            strAutoDenomResult = _("Found unconfirmed denominated outputs, will wait till they confirm to continue.");
            return false;
        }

        //don't use the queues all of the time for mixing
        if(nUseQueue > 33){

            // Look through the queues and see if anything matches
            BOOST_FOREACH(CAnonsendQueue& dsq, vecAnonsendQueue){
                CService addr;
                if(dsq.time == 0) continue;

                if(!dsq.GetAddress(addr)) continue;
                if(dsq.IsExpired()) continue;

                int protocolVersion;
                if(!dsq.GetProtocolVersion(protocolVersion)) continue;
                if(protocolVersion < MIN_PEER_PROTO_VERSION) continue;

                //non-denom's are incompatible
                if((dsq.nDenom & (1 << 4))) continue;

                //don't reuse inodes
                BOOST_FOREACH(CTxIn usedVin, vecInodesUsed){
                    if(dsq.vin == usedVin) {
                        continue;
                    }
                }

                // Try to match their denominations if possible
                if (!pwalletMain->SelectCoinsByDenominations(dsq.nDenom, nValueMin, nBalanceNeedsAnonymized, vCoins, vCoins2, nValueIn, 0, nAnonsendRounds)){
                    LogPrintf("DoAutomaticDenominating - Couldn't match denominations %d\n", dsq.nDenom);
                    continue;
                }

                // connect to inode and submit the queue request
                if(ConnectNode((CAddress)addr, NULL, true)){
                    submittedToInode = addr;

                    LOCK(cs_vNodes);
                    BOOST_FOREACH(CNode* pnode, vNodes)
                    {
                    	if((CNetAddr)pnode->addr != (CNetAddr)submittedToInode) continue;

                        std::string strReason;
                        if(txCollateral == CTransaction()){
                            if(!pwalletMain->CreateCollateralTransaction(txCollateral, strReason)){
                                LogPrintf("DoAutomaticDenominating -- dsa error:%s\n", strReason.c_str());
                                return false;
                            }
                        }

                        vecInodesUsed.push_back(dsq.vin);
                        sessionDenom = dsq.nDenom;

                        pnode->PushMessage("dsa", sessionDenom, txCollateral);
                        LogPrintf("DoAutomaticDenominating --- connected (from queue), sending dsa for %d %d - %s\n", sessionDenom, GetDenominationsByAmount(sessionTotalValue), pnode->addr.ToString().c_str());
                        strAutoDenomResult = "";
                        return true;
                    }
                } else {
                    LogPrintf("DoAutomaticDenominating --- error connecting \n");
                    strAutoDenomResult = _("Error connecting to inode.");
                    return DoAutomaticDenominating();
                }

                dsq.time = 0; //remove node
            }
        }

        //shuffle inodes around before we try to connect
        std::random_shuffle ( vecInodes.begin(), vecInodes.end() );
        int i = 0;

        // otherwise, try one randomly
        while(i < 10)
        {
            //don't reuse inodes
            BOOST_FOREACH(CTxIn usedVin, vecInodesUsed) {
                if(vecInodes[i].vin == usedVin){
                    i++;
                    continue;
                }
            }
            if(vecInodes[i].protocolVersion < MIN_PEER_PROTO_VERSION) {
                i++;
                continue;
            }

            if(vecInodes[i].nLastDsq != 0 &&
                vecInodes[i].nLastDsq + CountInodesAboveProtocol(anonSendPool.MIN_PEER_PROTO_VERSION)/5 > anonSendPool.nDsqCount){
                i++;
                continue;
            }

            lastTimeChanged = GetTimeMillis();
            LogPrintf("DoAutomaticDenominating -- attempt %d connection to inode %s\n", i, vecInodes[i].addr.ToString().c_str());
            if(ConnectNode((CAddress)vecInodes[i].addr, NULL, true)){
                submittedToInode = vecInodes[i].addr;

                LOCK(cs_vNodes);
                BOOST_FOREACH(CNode* pnode, vNodes)
                {
                    if((CNetAddr)pnode->addr != (CNetAddr)vecInodes[i].addr) continue;

                    std::string strReason;
                    if(txCollateral == CTransaction()){
                        if(!pwalletMain->CreateCollateralTransaction(txCollateral, strReason)){
                            LogPrintf("DoAutomaticDenominating -- create collateral error:%s\n", strReason.c_str());
                            return false;
                        }
                    }

                    vecInodesUsed.push_back(vecInodes[i].vin);

                    std::vector<int64_t> vecAmounts;
                    pwalletMain->ConvertList(vCoins, vecAmounts);
                    sessionDenom = GetDenominationsByAmounts(vecAmounts);

                    pnode->PushMessage("dsa", sessionDenom, txCollateral);
                    LogPrintf("DoAutomaticDenominating --- connected, sending dsa for %d - denom %d\n", sessionDenom, GetDenominationsByAmount(sessionTotalValue));
                    strAutoDenomResult = "";
                    return true;
                }
            } else {
                i++;
                continue;
            }
        }

        strAutoDenomResult = _("No compatible inode found.");
        return false;
    }

    strAutoDenomResult = "";
    if(!ready) return true;

    if(sessionDenom == 0) return true;

    return false;
}


bool CAnonSendPool::PrepareAnonsendDenominate()
{
    // Submit transaction to the pool if we get here, use sessionDenom so we use the same amount of money
    std::string strError = pwalletMain->PrepareAnonsendDenominate(0, nAnonsendRounds);
    LogPrintf("DoAutomaticDenominating : Running anonsend denominate. Return '%s'\n", strError.c_str());

    if(strError == "") return true;

    strAutoDenomResult = strError;
    LogPrintf("DoAutomaticDenominating : Error running denominate, %s\n", strError.c_str());
    return false;
}

bool CAnonSendPool::SendRandomPaymentToSelf()
{
    int64_t nBalance = pwalletMain->GetBalance();
    int64_t nPayment = (nBalance*0.35) + (rand() % nBalance);

    if(nPayment > nBalance) nPayment = nBalance-(0.1*COIN);

    // make our change address
    CReserveKey reservekey(pwalletMain);

    CScript scriptChange;
    CPubKey vchPubKey;
    assert(reservekey.GetReservedKey(vchPubKey)); // should never fail, as we just unlocked
    scriptChange= GetScriptForDestination(vchPubKey.GetID());

    CWalletTx wtx;
    int64_t nFeeRet = 0;
    std::string strFail = "";
    vector< pair<CScript, int64_t> > vecSend;

    // ****** Add fees ************ /
    vecSend.push_back(make_pair(scriptChange, nPayment));

    CCoinControl *coinControl=NULL;
    int32_t nChangePos;
    bool success = pwalletMain->CreateTransaction(vecSend, wtx, reservekey, nFeeRet, strDZeel, nChangePos, strFail, coinControl, ONLY_DENOMINATED);
    if(!success){
        LogPrintf("SendRandomPaymentToSelf: Error - %s\n", strFail.c_str());
        return false;
    }

    pwalletMain->CommitTransaction(wtx, reservekey);

    LogPrintf("SendRandomPaymentToSelf Success: tx %s\n", wtx.GetHash().GetHex().c_str());

    return true;
}

// Split up large inputs or create fee sized inputs
bool CAnonSendPool::MakeCollateralAmounts()
{
    // make our change address
    CReserveKey reservekey(pwalletMain);

    CScript scriptChange;
    CPubKey vchPubKey;
    assert(reservekey.GetReservedKey(vchPubKey)); // should never fail, as we just unlocked
    scriptChange= GetScriptForDestination(vchPubKey.GetID());

    CWalletTx wtx;
    int64_t nFeeRet = 0;
    std::string strFail = "";
    vector< pair<CScript, int64_t> > vecSend;

    vecSend.push_back(make_pair(scriptChange, (ANONSEND_COLLATERAL*2)+ANONSEND_FEE));
    vecSend.push_back(make_pair(scriptChange, (ANONSEND_COLLATERAL*2)+ANONSEND_FEE));

    CCoinControl *coinControl=NULL;
    int32_t nChangePos;
    // try to use non-denominated and not mn-like funds
    bool success = pwalletMain->CreateTransaction(vecSend, wtx, reservekey,
            nFeeRet, strDZeel, nChangePos, strFail, coinControl, ONLY_NONDENOMINATED_NOTIN);
    if(!success){
        // if we failed (most likeky not enough funds), try to use denominated instead -
        // MN-like funds should not be touched in any case and we can't mix denominated without collaterals anyway
        success = pwalletMain->CreateTransaction(vecSend, wtx, reservekey,
                nFeeRet, strDZeel, nChangePos, strFail, coinControl, ONLY_DENOMINATED);
        if(!success){
            LogPrintf("MakeCollateralAmounts: Error - %s\n", strFail.c_str());
            return false;
        }
    }

    // use the same cachedLastSuccess as for DS mixinx to prevent race
    if(pwalletMain->CommitTransaction(wtx, reservekey))
        cachedLastSuccess = pindexBest->nHeight;

    LogPrintf("MakeCollateralAmounts Success: tx %s\n", wtx.GetHash().GetHex().c_str());

    return true;
}

// Create denominations
bool CAnonSendPool::CreateDenominated(int64_t nTotalValue)
{
    // make our change address
    CReserveKey reservekey(pwalletMain);

    CScript scriptChange;
    CPubKey vchPubKey;
    assert(reservekey.GetReservedKey(vchPubKey)); // should never fail, as we just unlocked
    scriptChange= GetScriptForDestination(vchPubKey.GetID());

    CWalletTx wtx;
    int64_t nFeeRet = 0;
    std::string strFail = "";
    vector< pair<CScript, int64_t> > vecSend;
    int64_t nValueLeft = nTotalValue;

    // ****** Add collateral outputs ************ /
    if(!pwalletMain->HasCollateralInputs()) {
        vecSend.push_back(make_pair(scriptChange, (ANONSEND_COLLATERAL*2)+ANONSEND_FEE));
        nValueLeft -= (ANONSEND_COLLATERAL*2)+ANONSEND_FEE;
        vecSend.push_back(make_pair(scriptChange, (ANONSEND_COLLATERAL*2)+ANONSEND_FEE));
        nValueLeft -= (ANONSEND_COLLATERAL*2)+ANONSEND_FEE;
    }

    // ****** Add denoms ************ /
    BOOST_REVERSE_FOREACH(int64_t v, anonSendDenominations){
        int nOutputs = 0;

        // add each output up to 10 times until it can't be added again
        while(nValueLeft - v >= ANONSEND_FEE && nOutputs <= 10) {
            CScript scriptChange;
            CPubKey vchPubKey;
            //use a unique change address
            assert(reservekey.GetReservedKey(vchPubKey)); // should never fail, as we just unlocked
            scriptChange= GetScriptForDestination(vchPubKey.GetID());
            reservekey.KeepKey();

            vecSend.push_back(make_pair(scriptChange, v));

            //increment outputs and subtract denomination amount
            nOutputs++;
            nValueLeft -= v;
            LogPrintf("CreateDenominated1 %d\n", nValueLeft);
        }

        if(nValueLeft == 0) break;
    }
    LogPrintf("CreateDenominated2 %d\n", nValueLeft);

    // if we have anything left over, it will be automatically send back as change - there is no need to send it manually

    CCoinControl *coinControl=NULL;
    int32_t nChangePos;
    bool success = pwalletMain->CreateTransaction(vecSend, wtx, reservekey,
            nFeeRet, strDZeel, nChangePos, strFail, coinControl, ONLY_NONDENOMINATED_NOTIN);
    if(!success){
        LogPrintf("CreateDenominated: Error - %s\n", strFail.c_str());
        return false;
    }

    // use the same cachedLastSuccess as for DS mixinx to prevent race
    if(pwalletMain->CommitTransaction(wtx, reservekey))
        cachedLastSuccess = pindexBest->nHeight;

    LogPrintf("CreateDenominated Success: tx %s\n", wtx.GetHash().GetHex().c_str());

    return true;
}

bool CAnonSendPool::IsCompatibleWithEntries(std::vector<CTxOut> vout)
{
    BOOST_FOREACH(const CAnonSendEntry v, entries) {
        LogPrintf(" IsCompatibleWithEntries %d %d\n", GetDenominations(vout), GetDenominations(v.vout));
/*
        BOOST_FOREACH(CTxOut o1, vout)
            LogPrintf(" vout 1 - %s\n", o1.ToString().c_str());

        BOOST_FOREACH(CTxOut o2, v.vout)
            LogPrintf(" vout 2 - %s\n", o2.ToString().c_str());
*/
        if(GetDenominations(vout) != GetDenominations(v.vout)) return false;
    }

    return true;
}

bool CAnonSendPool::IsCompatibleWithSession(int64_t nDenom, CTransaction txCollateral, std::string& strReason)
{
    LogPrintf("CAnonSendPool::IsCompatibleWithSession - sessionDenom %d sessionUsers %d\n", sessionDenom, sessionUsers);

    if (!unitTest && !IsCollateralValid(txCollateral)){
        if(fDebug) LogPrintf ("CAnonSendPool::IsCompatibleWithSession - collateral not valid!\n");
        strReason = _("Collateral not valid.");
        return false;
    }

    if(sessionUsers < 0) sessionUsers = 0;

    if(sessionUsers == 0) {
        sessionDenom = nDenom;
        sessionUsers++;
        lastTimeChanged = GetTimeMillis();
        entries.clear();

        if(!unitTest){
            //broadcast that I'm accepting entries, only if it's the first entry though
            CAnonsendQueue dsq;
            dsq.nDenom = nDenom;
            dsq.vin = activeInode.vin;
            dsq.time = GetTime();
            dsq.Sign();
            dsq.Relay();
        }

        UpdateState(POOL_STATUS_QUEUE);
        vecSessionCollateral.push_back(txCollateral);
        return true;
    }

    if((state != POOL_STATUS_ACCEPTING_ENTRIES && state != POOL_STATUS_QUEUE) || sessionUsers >= GetMaxPoolTransactions()){
        if((state != POOL_STATUS_ACCEPTING_ENTRIES && state != POOL_STATUS_QUEUE)) strReason = _("Incompatible mode.");
        if(sessionUsers >= GetMaxPoolTransactions()) strReason = _("Inode queue is full.");
        LogPrintf("CAnonSendPool::IsCompatibleWithSession - incompatible mode, return false %d %d\n", state != POOL_STATUS_ACCEPTING_ENTRIES, sessionUsers >= GetMaxPoolTransactions());
        return false;
    }

    if(nDenom != sessionDenom) {
        strReason = _("No matching denominations found for mixing.");
        return false;
    }

    LogPrintf("CAnonSendPool::IsCompatibleWithSession - compatible\n");

    sessionUsers++;
    lastTimeChanged = GetTimeMillis();
    vecSessionCollateral.push_back(txCollateral);

    return true;
}

//create a nice string to show the denominations
void CAnonSendPool::GetDenominationsToString(int nDenom, std::string& strDenom){
    // Function returns as follows:
    //
    // bit 0 - 100NAV+1 ( bit on if present )
    // bit 1 - 10NAV+1
    // bit 2 - 1NAV+1
    // bit 3 - .1NAV+1
    // bit 3 - non-denom


    strDenom = "";

    if(nDenom & (1 << 0)) {
        if(strDenom.size() > 0) strDenom += "+";
        strDenom += "100";
    }

    if(nDenom & (1 << 1)) {
        if(strDenom.size() > 0) strDenom += "+";
        strDenom += "10";
    }

    if(nDenom & (1 << 2)) {
        if(strDenom.size() > 0) strDenom += "+";
        strDenom += "1";
    }

    if(nDenom & (1 << 3)) {
        if(strDenom.size() > 0) strDenom += "+";
        strDenom += "0.1";
    }
}

// return a bitshifted integer representing the denominations in this list
int CAnonSendPool::GetDenominations(const std::vector<CTxOut>& vout){
    std::vector<pair<int64_t, int> > denomUsed;

    // make a list of denominations, with zero uses
    BOOST_FOREACH(int64_t d, anonSendDenominations)
        denomUsed.push_back(make_pair(d, 0));

    // look for denominations and update uses to 1
    BOOST_FOREACH(CTxOut out, vout){
        bool found = false;
        BOOST_FOREACH (PAIRTYPE(int64_t, int)& s, denomUsed){
            if (out.nValue == s.first){
                s.second = 1;
                found = true;
            }
        }
        if(!found) return 0;
    }

    int denom = 0;
    int c = 0;
    // if the denomination is used, shift the bit on.
    // then move to the next
    BOOST_FOREACH (PAIRTYPE(int64_t, int)& s, denomUsed)
        denom |= s.second << c++;

    // Function returns as follows:
    //
    // bit 0 - 100NAV+1 ( bit on if present )
    // bit 1 - 10NAV+1
    // bit 2 - 1NAV+1
    // bit 3 - .1NAV+1

    return denom;
}


int CAnonSendPool::GetDenominationsByAmounts(std::vector<int64_t>& vecAmount){
    CScript e = CScript();
    std::vector<CTxOut> vout1;

    // Make outputs by looping through denominations, from small to large
    BOOST_REVERSE_FOREACH(int64_t v, vecAmount){
        int nOutputs = 0;

        CTxOut o(v, e);
        vout1.push_back(o);
        nOutputs++;
    }

    return GetDenominations(vout1);
}

int CAnonSendPool::GetDenominationsByAmount(int64_t nAmount, int nDenomTarget){
    CScript e = CScript();
    int64_t nValueLeft = nAmount;

    std::vector<CTxOut> vout1;

    // Make outputs by looping through denominations, from small to large
    BOOST_REVERSE_FOREACH(int64_t v, anonSendDenominations){
        if(nDenomTarget != 0){
            bool fAccepted = false;
            if((nDenomTarget & (1 << 0)) &&      v == ((100000*COIN)+100000000)) {fAccepted = true;}
            else if((nDenomTarget & (1 << 1)) && v == ((10000*COIN) +10000000)) {fAccepted = true;}
            else if((nDenomTarget & (1 << 2)) && v == ((1000*COIN)  +1000000)) {fAccepted = true;}
            else if((nDenomTarget & (1 << 3)) && v == ((100*COIN)   +100000)) {fAccepted = true;}
            else if((nDenomTarget & (1 << 4)) && v == ((10*COIN)    +10000)) {fAccepted = true;}
            else if((nDenomTarget & (1 << 5)) && v == ((1*COIN)     +1000)) {fAccepted = true;}
            else if((nDenomTarget & (1 << 6)) && v == ((.1*COIN)    +100)) {fAccepted = true;}
            if(!fAccepted) continue;
        }

        int nOutputs = 0;

        // add each output up to 10 times until it can't be added again
        while(nValueLeft - v >= 0 && nOutputs <= 10) {
            CTxOut o(v, e);
            vout1.push_back(o);
            nValueLeft -= v;
            nOutputs++;
        }
        LogPrintf("GetDenominationsByAmount --- %d nOutputs %d\n", v, nOutputs);
    }

    //add non-denom left overs as change
    if(nValueLeft > 0){
        CTxOut o(nValueLeft, e);
        vout1.push_back(o);
    }

    return GetDenominations(vout1);
}

bool CAnonSendSigner::IsVinAssociatedWithPubkey(CTxIn& vin, CPubKey& pubkey){
    CScript payee2;
    payee2= GetScriptForDestination(pubkey.GetID());

    CTransaction txVin;
    uint256 hash;
    //if(GetTransaction(vin.prevout.hash, txVin, hash, true)){
    if(GetTransaction(vin.prevout.hash, txVin, hash)){
        BOOST_FOREACH(CTxOut out, txVin.vout){
            if(out.nValue == 100000*COIN){
                if(out.scriptPubKey == payee2) return true;
            }
        }
    }

    return false;
}

bool CAnonSendSigner::SetKey(std::string strSecret, std::string& errorMessage, CKey& key, CPubKey& pubkey){
    CBitcoinSecret vchSecret;
    bool fGood = vchSecret.SetString(strSecret);

    if (!fGood) {
        errorMessage = _("Invalid private key.");
        return false;
    }

    key = vchSecret.GetKey();
    pubkey = key.GetPubKey();

    return true;
}

bool CAnonSendSigner::SignMessage(std::string strMessage, std::string& errorMessage, vector<unsigned char>& vchSig, CKey key)
{
    CHashWriter ss(SER_GETHASH, 0);
    ss << strMessageMagic;
    ss << strMessage;

    if (!key.SignCompact(ss.GetHash(), vchSig)) {
        errorMessage = _("Signing failed.");
        return false;
    }

    return true;
}

bool CAnonSendSigner::VerifyMessage(CPubKey pubkey, vector<unsigned char>& vchSig, std::string strMessage, std::string& errorMessage)
{
    CHashWriter ss(SER_GETHASH, 0);
    ss << strMessageMagic;
    ss << strMessage;

    CPubKey pubkey2;
    if (!pubkey2.RecoverCompact(ss.GetHash(), vchSig)) {
        errorMessage = _("Error recovering public key.");
        return false;
    }

    if (fDebug && pubkey2.GetID() != pubkey.GetID())
        LogPrintf("CAnonSendSigner::VerifyMessage -- keys don't match: %s %s", pubkey2.GetID().ToString(), pubkey.GetID().ToString());

    return (pubkey2.GetID() == pubkey.GetID());
}

bool CAnonsendQueue::Sign()
{
    if(!fINode) return false;

    std::string strMessage = vin.ToString() + boost::lexical_cast<std::string>(nDenom) + boost::lexical_cast<std::string>(time) + boost::lexical_cast<std::string>(ready);

    CKey key2;
    CPubKey pubkey2;
    std::string errorMessage = "";

    if(!anonSendSigner.SetKey(strINodePrivKey, errorMessage, key2, pubkey2))
    {
        LogPrintf("CAnonsendQueue():Relay - ERROR: Invalid inodeprivkey: '%s'\n", errorMessage.c_str());
        return false;
    }

    if(!anonSendSigner.SignMessage(strMessage, errorMessage, vchSig, key2)) {
        LogPrintf("CAnonsendQueue():Relay - Sign message failed");
        return false;
    }

    if(!anonSendSigner.VerifyMessage(pubkey2, vchSig, strMessage, errorMessage)) {
        LogPrintf("CAnonsendQueue():Relay - Verify message failed");
        return false;
    }

    return true;
}

bool CAnonsendQueue::Relay()
{

    LOCK(cs_vNodes);
    BOOST_FOREACH(CNode* pnode, vNodes){
        // always relay to everyone
        pnode->PushMessage("dsq", (*this));
    }

    return true;
}

bool CAnonsendQueue::CheckSignature()
{
    BOOST_FOREACH(CINode& mn, vecInodes) {

        if(mn.vin == vin) {
            std::string strMessage = vin.ToString() + boost::lexical_cast<std::string>(nDenom) + boost::lexical_cast<std::string>(time) + boost::lexical_cast<std::string>(ready);

            std::string errorMessage = "";
            if(!anonSendSigner.VerifyMessage(mn.pubkey2, vchSig, strMessage, errorMessage)){
                return error("CAnonsendQueue::CheckSignature() - Got bad inode address signature %s \n", vin.ToString().c_str());
            }

            return true;
        }
    }

    return false;
}


//TODO: Rename/move to core
void ThreadCheckAnonSendPool()
{
    if(fLiteMode) return; //disable all anonsend/inode related functionality

    // Make this thread recognisable as the wallet flushing thread
    RenameThread("navcoin-anonsend");

    unsigned int c = 0;
    std::string errorMessage;

    while (true)
    {
        c++;

        MilliSleep(2500);
        //LogPrintf("ThreadCheckAnonSendPool::check timeout\n");
        anonSendPool.CheckTimeout();

        if(c % 60 == 0){
            LOCK(cs_main);
            /*
                cs_main is required for doing inode.Check because something
                is modifying the coins view without a mempool lock. It causes
                segfaults from this code without the cs_main lock.
            */
	    {

	    LOCK(cs_inodes);
            vector<CINode>::iterator it = vecInodes.begin();
            //check them separately
            while(it != vecInodes.end()){
                (*it).Check();
                ++it;
            }

            //remove inactive
            it = vecInodes.begin();
            while(it != vecInodes.end()){
                if((*it).enabled == 4 || (*it).enabled == 3){
                    LogPrintf("Removing inactive inode %s\n", (*it).addr.ToString().c_str());
                    it = vecInodes.erase(it);
                } else {
                    ++it;
                }
            }

	    }

            inodePayments.CleanPaymentList();
            CleanTransactionLocksList();
        }

        //try to sync the inode list and payment list every 5 seconds from at least 3 nodes
        if(c % 5 == 0 && RequestedINodeList < 3){
            bool fIsInitialDownload = IsInitialBlockDownload();
            if(!fIsInitialDownload) {
                LOCK(cs_vNodes);
                BOOST_FOREACH(CNode* pnode, vNodes)
                {
                    if (pnode->nVersion >= anonSendPool.MIN_PEER_PROTO_VERSION) {

                        //keep track of who we've asked for the list
                        if(pnode->HasFulfilledRequest("mnsync")) continue;
                        pnode->FulfilledRequest("mnsync");

                        LogPrintf("Successfully synced, asking for Inode list and payment list\n");

                        pnode->PushMessage("dseg", CTxIn()); //request full mn list
                        pnode->PushMessage("mnget"); //sync payees
                        pnode->PushMessage("gethubs"); //get current network sporks
                        RequestedINodeList++;
                    }
                }
            }
        }

        if(c % INODE_PING_SECONDS == 0){
            activeInode.ManageStatus();
        }

        if(c % 60 == 0){
            //if we've used 1/5 of the inode list, then clear the list.
            if((int)vecInodesUsed.size() > (int)vecInodes.size() / 5)
                vecInodesUsed.clear();
        }

        //auto denom every 1 minutes (liquidity provides try less often)
        if(c % 60*(nLiquidityProvider+1) == 0){
            if(nLiquidityProvider!=0){
                int nRand = rand() % (101+nLiquidityProvider);
                //about 1/100 chance of starting over after 4 rounds.
                if(nRand == 50+nLiquidityProvider && pwalletMain->GetAverageAnonymizedRounds() > 8){
                    anonSendPool.SendRandomPaymentToSelf();
                    int nLeftToAnon = ((pwalletMain->GetBalance() - pwalletMain->GetAnonymizedBalance())/COIN)-3;
                    if(nLeftToAnon > 999) nLeftToAnon = 999;
                    nAnonymizeNavCoinAmount = (rand() % nLeftToAnon)+3;
                } else {
                    anonSendPool.DoAutomaticDenominating();
                }
            } else {
                anonSendPool.DoAutomaticDenominating();
            }
        }
    }
}