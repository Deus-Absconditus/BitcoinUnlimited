// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2015 The Bitcoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// NOTE: This file is intended to be customised by the end user, and includes only local node policy logic

#include "policy/policy.h"

#include "main.h"
#include "tinyformat.h"
#include "util.h"
#include "utilstrencodings.h"

/**
 * Check transaction inputs to mitigate two
 * potential denial-of-service attacks:
 *
 * 1. scriptSigs with extra data stuffed into them,
 *    not consumed by scriptPubKey (or P2SH script)
 * 2. P2SH scripts with a crazy number of expensive
 *    CHECKSIG/CHECKMULTISIG operations
 *
 * Check transaction inputs, and make sure any
 * pay-to-script-hash transactions are evaluating IsStandard scripts
 *
 * Why bother? To avoid denial-of-service attacks; an attacker
 * can submit a standard HASH... OP_EQUAL transaction,
 * which will get accepted into blocks. The redemption
 * script can be anything; an attacker could use a very
 * expensive-to-check-upon-redemption script like:
 *   DUP CHECKSIG DROP ... repeated 100 times... OP_1
 */

bool IsStandard(const CScript &scriptPubKey, txnouttype &whichType)
{
    std::vector<std::vector<unsigned char> > vSolutions;
    if (!Solver(scriptPubKey, whichType, vSolutions))
        return false;

    if (whichType == TX_MULTISIG)
    {
        unsigned char m = vSolutions.front()[0];
        unsigned char n = vSolutions.back()[0];
        // Support up to x-of-3 multisig txns as standard
        if (n < 1 || n > 3)
            return false;
        if (m < 1 || m > n)
            return false;
    }
    else if (whichType == TX_CLTV)
    {
        return true; // CLTV Freeze are standard enable(disable)
    }
    else if (whichType == TX_NULL_DATA || whichType == TX_LABELPUBLIC)
    {
        if (!fAcceptDatacarrier || scriptPubKey.size() > nMaxDatacarrierBytes)
            return false;
    }

    return whichType != TX_NONSTANDARD;
}

bool IsStandardTx(const CTransactionRef &tx, std::string &reason)
{
    if (tx->nVersion > CTransaction::MAX_STANDARD_VERSION || tx->nVersion < 1)
    {
        reason = "version";
        return false;
    }

    // Extremely large transactions with lots of inputs can cost the network
    // almost as much to process as they cost the sender in fees, because
    // computing signature hashes is O(ninputs*txsize). Limiting transactions
    // to MAX_STANDARD_TX_SIZE mitigates CPU exhaustion attacks.
    if (tx->GetTxSize() >= MAX_STANDARD_TX_SIZE)
    {
        reason = "tx-size";
        return false;
    }

    for (const CTxIn &txin : tx->vin)
    {
        // Biggest 'standard' txin is a 15-of-15 P2SH multisig with compressed
        // keys. (remember the 520 byte limit on redeemScript size) That works
        // out to a (15*(33+1))+3=513 byte redeemScript, 513+1+15*(73+1)+3=1627
        // bytes of scriptSig, which we round off to 1650 bytes for some minor
        // future-proofing. That's also enough to spend a 20-of-20
        // CHECKMULTISIG scriptPubKey, though such a scriptPubKey is not
        // considered standard)
        if (txin.scriptSig.size() > 1650)
        {
            reason = "scriptsig-size";
            return false;
        }
        if (!txin.scriptSig.IsPushOnly())
        {
            reason = "scriptsig-not-pushonly";
            return false;
        }
    }

    unsigned int nDataOut = 0;
    txnouttype whichType;
    for (const CTxOut &txout : tx->vout)
    {
        if (!::IsStandard(txout.scriptPubKey, whichType))
        {
            reason = "scriptpubkey";
            return false;
        }

        if ((whichType == TX_NULL_DATA) || (whichType == TX_LABELPUBLIC))
            nDataOut++;
        else if ((whichType == TX_MULTISIG) && (!fIsBareMultisigStd))
        {
            reason = "bare-multisig";
            return false;
        }
        else if (txout.IsDust())
        {
            reason = "dust";
            return false;
        }
    }

    // only one OP_RETURN txout is permitted
    if (nDataOut > 1)
    {
        reason = "multi-op-return";
        return false;
    }

    return true;
}

bool AreInputsStandard(const CTransactionRef &tx, const CCoinsViewCache &mapInputs)
{
    if (tx->IsCoinBase())
        return true; // Coinbases don't use vin normally

    for (unsigned int i = 0; i < tx->vin.size(); i++)
    {
        txnouttype whichType;
        {
            CoinAccessor coin(mapInputs, tx->vin[i].prevout);
            const CTxOut &prev = coin->out;

            std::vector<std::vector<unsigned char> > vSolutions;
            // get the scriptPubKey corresponding to this input:
            const CScript &prevScript = prev.scriptPubKey;
            if (!Solver(prevScript, whichType, vSolutions))
                return false;
        }

        if (whichType == TX_SCRIPTHASH)
        {
            std::vector<std::vector<unsigned char> > stack;
            // convert the scriptSig into a stack, so we can inspect the redeemScript
            // This is only parsing the scriptSig which should not have any non-push opcodes in it anyway,
            // and it matches the P2SH script template, so we know that it won't have any ops, only pushes
            // so pass MAX_OPS_PER_SCRIPT for the max number of ops to match prior behavior exactly
            if (!EvalScript(
                    stack, tx->vin[i].scriptSig, SCRIPT_VERIFY_NONE, MAX_OPS_PER_SCRIPT, BaseSignatureChecker(), 0))
                return false;
            if (stack.empty())
                return false;
            CScript subscript(stack.back().begin(), stack.back().end());
            if (subscript.GetSigOpCount(STANDARD_CHECKDATASIG_VERIFY_FLAGS, true) > MAX_P2SH_SIGOPS)
            {
                return false;
            }
        }
    }

    return true;
}
