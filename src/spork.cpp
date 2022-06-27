// Copyright (c) 2014-2019 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <spork.h>

#include <base58.h>
#include <chainparams.h>
#include <validation.h>
#include <messagesigner.h>
#include <net_processing.h>
#include <netmessagemaker.h>
#include <util/message.h>
#include <string>
#include <key_io.h>
#include <timedata.h>
#include <util/ranges.h>
const std::string CSporkManager::SERIALIZATION_VERSION_STRING = "CSporkManager-Version-2";

CSporkManager sporkManager;

bool CSporkManager::SporkValueIsActive(int32_t nSporkID, int64_t& nActiveValueRet) const
{
    LOCK(cs);

    if (!mapSporksActive.count(nSporkID)) return false;

    if (auto it = mapSporksCachedValues.find(nSporkID); it != mapSporksCachedValues.end()) {
        nActiveValueRet = it->second;
        return true;
    }

    // calc how many values we have and how many signers vote for every value
    std::unordered_map<int64_t, int> mapValueCounts;
    for (const auto& [_, spork] : mapSporksActive.at(nSporkID)) {
        mapValueCounts[spork.nValue]++;
        if (mapValueCounts.at(spork.nValue) >= nMinSporkKeys) {
            // nMinSporkKeys is always more than the half of the max spork keys number,
            // so there is only one such value and we can stop here
            nActiveValueRet = spork.nValue;
            mapSporksCachedValues[nSporkID] = nActiveValueRet;
            return true;
        }
    }

    return false;
}

void CSporkManager::Clear()
{
    LOCK(cs);
    mapSporksActive.clear();
    mapSporksByHash.clear();
    // sporkPubKeyID and sporkPrivKey should be set in init.cpp,
    // we should not alter them here.
}

void CSporkManager::CheckAndRemove()
{
    LOCK(cs);
    assert(!setSporkPubKeyIDs.empty());

    for (auto itActive = mapSporksActive.begin(); itActive != mapSporksActive.end();) {
        auto itSignerPair = itActive->second.begin();
        while (itSignerPair != itActive->second.end()) {
            bool fHasValidSig = setSporkPubKeyIDs.find(itSignerPair->first) != setSporkPubKeyIDs.end() &&
                                itSignerPair->second.CheckSignature(itSignerPair->first);
            if (!fHasValidSig) {
                mapSporksByHash.erase(itSignerPair->second.GetHash());
                itActive->second.erase(itSignerPair++);
                continue;
            }
            ++itSignerPair;
        }
        if (itActive->second.empty()) {
            mapSporksActive.erase(itActive++);
            continue;
        }
        ++itActive;
    }

    for (auto itByHash = mapSporksByHash.begin(); itByHash != mapSporksByHash.end();) {
        bool found = false;
        for (const auto& signer : setSporkPubKeyIDs) {
            if (itByHash->second.CheckSignature(signer)) {
                found = true;
                break;
            }
        }
        if (!found) {
            mapSporksByHash.erase(itByHash++);
            continue;
        }
        ++itByHash;
    }
}

void CSporkManager::ProcessSporkMessages(CNode* pfrom, const std::string& strCommand, CDataStream& vRecv, CConnman& connman, PeerManager& peerman)
{
    ProcessSpork(pfrom, strCommand, vRecv, peerman);
    ProcessGetSporks(pfrom, strCommand, connman);
}

void CSporkManager::ProcessSpork(CNode* pfrom, const std::string& strCommand, CDataStream& vRecv, PeerManager& peerman)
{
    if (strCommand != NetMsgType::SPORK) return;

    CSporkMessage spork;
    vRecv >> spork;

    const uint256 &hash = spork.GetHash();
    std::string strLogMsg;
    PeerRef peer = peerman.GetPeerRef(pfrom->GetId());
    if (peer)
        peerman.AddKnownTx(*peer, hash);
    {
        LOCK(cs_main);
        peerman.ReceivedResponse(pfrom->GetId(), hash);
        strLogMsg = strprintf("SPORK -- hash: %s id: %d value: %10d peer=%d", hash.ToString(), spork.nSporkID, spork.nValue, pfrom->GetId());
    }

    if (spork.nTimeSigned > GetAdjustedTime() + 2 * 60 * 60) {
        {
            LOCK(cs_main);
            peerman.ForgetTxHash(pfrom->GetId(), hash);
        }
        LogPrint(BCLog::SPORK, "CSporkManager::ProcessSpork -- ERROR: too far into the future\n");
        if(peer)
            peerman.Misbehaving(*peer, 100, "spork too far into the future");
        return;
    }

    CKeyID keyIDSigner;

        if (!spork.GetSignerKeyID(keyIDSigner) || !setSporkPubKeyIDs.count(keyIDSigner)) {
            {
                LOCK(cs_main);
                peerman.ForgetTxHash(pfrom->GetId(), hash);
            }
            LogPrint(BCLog::SPORK, "CSporkManager::ProcessSpork -- ERROR: invalid signature\n");
            if(peer)
                peerman.Misbehaving(*peer, 100, "invalid spork signature");
            return;
        }
        bool bSeen = false;
        {
            LOCK(cs); // make sure to not lock this together with cs_main
            if (mapSporksActive.count(spork.nSporkID)) {
                if (mapSporksActive[spork.nSporkID].count(keyIDSigner)) {
                    if (mapSporksActive[spork.nSporkID][keyIDSigner].nTimeSigned >= spork.nTimeSigned) {
                        LogPrint(BCLog::SPORK, "%s seen\n", strLogMsg);
                        bSeen = true;
                    }
                } else {
                    LogPrintf("%s updated\n", strLogMsg);
                }
            } else {
                LogPrintf("%s new signer\n", strLogMsg);
            }
        }
    if(bSeen) {
        LOCK(cs_main);
        peerman.ForgetTxHash(pfrom->GetId(), hash);
        return;
    }


    {
        LOCK(cs); // make sure to not lock this together with cs_main
        mapSporksByHash[hash] = spork;
        mapSporksActive[spork.nSporkID][keyIDSigner] = spork;
        // Clear cached values on new spork being processed
        mapSporksCachedActive.erase(spork.nSporkID);
        mapSporksCachedValues.erase(spork.nSporkID);
    }
    spork.Relay(peerman);
    {
        LOCK(cs_main);
        peerman.ForgetTxHash(pfrom->GetId(), hash);
    }
}

void CSporkManager::ProcessGetSporks(CNode* pfrom, const std::string &strCommand, CConnman& connman)
{
    if (strCommand != NetMsgType::GETSPORKS) return;

    LOCK(cs); // make sure to not lock this together with cs_main
    for (const auto& pair : mapSporksActive) {
        for (const auto& signerSporkPair: pair.second) {
            connman.PushMessage(pfrom, CNetMsgMaker(pfrom->GetCommonVersion()).Make(NetMsgType::SPORK, signerSporkPair.second));
        }
    }
}

bool CSporkManager::UpdateSpork(int32_t nSporkID, int64_t nValue, PeerManager& peerman)
{
    CSporkMessage spork(nSporkID, nValue, GetAdjustedTime());


    if (!spork.Sign(sporkPrivKey)) {
        LogPrintf("CSporkManager::%s -- ERROR: signing failed for spork %d\n", __func__, nSporkID);
        return false;
    }

    CKeyID keyIDSigner;
    if (!spork.GetSignerKeyID(keyIDSigner) || !setSporkPubKeyIDs.count(keyIDSigner)) {
        LogPrintf("CSporkManager::UpdateSpork: failed to find keyid for private key\n");
        return false;
    }

    LogPrintf("CSporkManager::%s -- signed %d %s\n", __func__, nSporkID, spork.GetHash().ToString());
    {
        LOCK(cs);
        mapSporksByHash[spork.GetHash()] = spork;
        mapSporksActive[nSporkID][keyIDSigner] = spork;
        // Clear cached values on new spork being processed
        mapSporksCachedActive.erase(spork.nSporkID);
        mapSporksCachedValues.erase(spork.nSporkID);
    }
    spork.Relay(peerman);
    return true;
}

bool CSporkManager::IsSporkActive(int32_t nSporkID) const
{
    LOCK(cs);
    // If nSporkID is cached, and the cached value is true, then return early true
    if (auto it = mapSporksCachedActive.find(nSporkID); it != mapSporksCachedActive.end() && it->second) {
        return true;
    }

    int64_t nSporkValue = GetSporkValue(nSporkID);
    // Get time is somewhat costly it looks like
    bool ret = nSporkValue < GetAdjustedTime();
    // Only cache true values
    if (ret) {
        mapSporksCachedActive[nSporkID] = ret;
    }
    return ret;
}

int64_t CSporkManager::GetSporkValue(int32_t nSporkID) const
{
    LOCK(cs);

    if (int64_t nSporkValue = -1; SporkValueIsActive(nSporkID, nSporkValue)) {
        return nSporkValue;
    }


    if (auto optSpork = ranges::find_if_opt(sporkDefs,
                                            [&nSporkID](const auto& sporkDef){return sporkDef.sporkId == nSporkID;})) {
        return optSpork->defaultValue;
    } else {
        LogPrint(BCLog::SPORK, "CSporkManager::GetSporkValue -- Unknown Spork ID %d\n", nSporkID);
        return -1;
    }
}

int32_t CSporkManager::GetSporkIDByName(const std::string& strName)
{
    if (auto optSpork = ranges::find_if_opt(sporkDefs,
                                            [&strName](const auto& sporkDef){return sporkDef.name == strName;})) {
        return optSpork->sporkId;
    }

    LogPrint(BCLog::SPORK, "CSporkManager::GetSporkIDByName -- Unknown Spork name '%s'\n", strName);
    return SPORK_INVALID;
}

bool CSporkManager::GetSporkByHash(const uint256& hash, CSporkMessage& sporkRet) const
{
    LOCK(cs);

    const auto it = mapSporksByHash.find(hash);

    if (it == mapSporksByHash.end())
        return false;

    sporkRet = it->second;

    return true;
}

bool CSporkManager::SetSporkAddress(const std::string& strAddress)
{
    LOCK(cs);
    CTxDestination dest = DecodeDestination(strAddress);
    CKeyID keyID;
    if (auto witness_id = std::get_if<WitnessV0KeyHash>(&dest)) {	
        keyID = ToKeyID(*witness_id);
    }	
    else if (auto key_id = std::get_if<PKHash>(&dest)) {	
        keyID = ToKeyID(*key_id);
    }	
    if (keyID.IsNull()) {
        LogPrintf("CSporkManager::SetSporkAddress -- Failed to parse spork address\n");
        return false;
    }
    setSporkPubKeyIDs.insert(keyID);
    return true;
}

bool CSporkManager::SetMinSporkKeys(int minSporkKeys)
{
    int maxKeysNumber = setSporkPubKeyIDs.size();
    if ((minSporkKeys <= maxKeysNumber / 2) || (minSporkKeys > maxKeysNumber)) {
        LogPrintf("CSporkManager::SetMinSporkKeys -- Invalid min spork signers number: %d\n", minSporkKeys);
        return false;
    }
    nMinSporkKeys = minSporkKeys;
    return true;
}

bool CSporkManager::SetPrivKey(const std::string& strPrivKey)
{
    CKey key;
    CPubKey pubKey;
    if (!CMessageSigner::GetKeysFromSecret(strPrivKey, key, pubKey)) {
        LogPrintf("CSporkManager::SetPrivKey -- Failed to parse private key\n");
        return false;
    }

    if (setSporkPubKeyIDs.find(pubKey.GetID()) == setSporkPubKeyIDs.end()) {
        LogPrintf("CSporkManager::SetPrivKey -- New private key does not belong to spork addresses\n");
        return false;
    }

    if (!CSporkMessage().Sign(key)) {
        LogPrintf("CSporkManager::SetPrivKey -- Test signing failed\n");
        return false;
    }

    // Test signing successful, proceed
    LOCK(cs);
    LogPrintf("CSporkManager::SetPrivKey -- Successfully initialized as spork signer\n");
    sporkPrivKey = key;
    return true;
}

std::string CSporkManager::ToString() const
{
    LOCK(cs);
    return strprintf("Sporks: %llu", mapSporksActive.size());
}

uint256 CSporkMessage::GetHash() const
{
    return SerializeHash(*this);
}

uint256 CSporkMessage::GetSignatureHash() const
{
    CHashWriter s(SER_GETHASH, 0);
    s << nSporkID;
    s << nValue;
    s << nTimeSigned;
    return s.GetHash();
}

bool CSporkMessage::Sign(const CKey& key)
{
    if (!key.IsValid()) {
        LogPrintf("CSporkMessage::Sign -- signing key is not valid\n");
        return false;
    }

    CKeyID pubKeyId = key.GetPubKey().GetID();


    uint256 hash = GetSignatureHash();

    if(!CHashSigner::SignHash(hash, key, vchSig)) {
        LogPrintf("CSporkMessage::Sign -- SignHash() failed\n");
        return false;
    }

    if (!CHashSigner::VerifyHash(hash, pubKeyId, vchSig)) {
        LogPrintf("CSporkMessage::Sign -- VerifyHash() failed\n");
        return false;
    }
  

    return true;
}

bool CSporkMessage::CheckSignature(const CKeyID& pubKeyId) const
{
    const uint256 &hash = GetSignatureHash();

    if (!CHashSigner::VerifyHash(hash, pubKeyId, vchSig)) {
        LogPrint(BCLog::SPORK, "CSporkMessage::CheckSignature -- VerifyHash() failed\n");
        return false;
    }
    return true;
}

bool CSporkMessage::GetSignerKeyID(CKeyID& retKeyidSporkSigner) const
{
    CPubKey pubkeyFromSig;
    if (!pubkeyFromSig.RecoverCompact(GetSignatureHash(), vchSig)) {
        return false;
    }

    retKeyidSporkSigner = pubkeyFromSig.GetID();
    return true;
}

void CSporkMessage::Relay(PeerManager& peerman) const
{
    CInv inv(MSG_SPORK, GetHash());
    peerman.RelayTransactionOther(inv);
}
