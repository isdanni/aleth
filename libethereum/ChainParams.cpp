// Aleth: Ethereum C++ client, tools and libraries.
// Copyright 2015-2019 Aleth Authors.
// Licensed under the GNU General Public License, Version 3.

#include "ChainParams.h"
#include "Account.h"
#include "GenesisInfo.h"
#include "State.h"
#include "ValidationSchemes.h"
#include <json_spirit/JsonSpiritHeaders.h>
#include <libdevcore/JsonUtils.h>
#include <libdevcore/Log.h>
#include <libdevcore/TrieDB.h>
#include <libethcore/BlockHeader.h>
#include <libethcore/Precompiled.h>
#include <libethcore/SealEngine.h>

using namespace std;
using namespace dev;
using namespace eth;
using namespace eth::validation;
namespace js = json_spirit;

ChainParams::ChainParams()
{
    for (unsigned i = 1; i <= 4; ++i)
        genesisState[Address(i)] = Account(0, 1);
    // Setup default precompiled contracts as equal to genesis of Frontier.
    precompiled.insert(make_pair(Address(1), PrecompiledContract(3000, 0, PrecompiledRegistrar::executor("ecrecover"))));
    precompiled.insert(make_pair(Address(2), PrecompiledContract(60, 12, PrecompiledRegistrar::executor("sha256"))));
    precompiled.insert(make_pair(Address(3), PrecompiledContract(600, 120, PrecompiledRegistrar::executor("ripemd160"))));
    precompiled.insert(make_pair(Address(4), PrecompiledContract(15, 3, PrecompiledRegistrar::executor("identity"))));
}

ChainParams::ChainParams(string const& _json, h256 const& _stateRoot)
{
    *this = loadConfig(_json, _stateRoot);
}

ChainParams ChainParams::loadConfig(
        string const& _json, h256 const& _stateRoot, const boost::filesystem::path& _configPath) const
{
    ChainParams cp(*this);
    js::mValue val;

    try
    {
        js::read_string_or_throw(_json, val);
    }
    catch (js::Error_position const& error)
    {
        std::string const comment = "json parsing error detected on line " +
                                    std::to_string(error.line_) + " in column " +
                                    std::to_string(error.column_) + ": " + error.reason_;
        std::cerr << comment << "\n";
        BOOST_THROW_EXCEPTION(SyntaxError() << errinfo_comment(comment));
    }

    js::mObject obj = val.get_obj();

    validateConfigJson(obj);

    // params
    cp.sealEngineName = obj[c_sealEngine].get_str();
    js::mObject params = obj[c_params].get_obj();

    // Params that are not required and could be set to default value
    if (params.count(c_accountStartNonce))
        cp.accountStartNonce = fromBigEndian<u256>(fromHex(params[c_accountStartNonce].get_str()));
    if (params.count(c_maximumExtraDataSize))
        cp.maximumExtraDataSize =
            fromBigEndian<u256>(fromHex(params[c_maximumExtraDataSize].get_str()));

    cp.tieBreakingGas = params.count(c_tieBreakingGas) ? params[c_tieBreakingGas].get_bool() : true;
    if (params.count(c_blockReward))
        cp.setBlockReward(fromBigEndian<u256>(fromHex(params[c_blockReward].get_str())));

    auto setOptionalU256Parameter = [&params](u256 &_destination, string const& _name)
    {
        if (params.count(_name))
            _destination = fromBigEndian<u256>(fromHex(params.at(_name).get_str()));
    };
    setOptionalU256Parameter(cp.minGasLimit, c_minGasLimit);
    setOptionalU256Parameter(cp.maxGasLimit, c_maxGasLimit);
    setOptionalU256Parameter(cp.gasLimitBoundDivisor, c_gasLimitBoundDivisor);
    setOptionalU256Parameter(cp.homesteadForkBlock, c_homesteadForkBlock);
    setOptionalU256Parameter(cp.EIP150ForkBlock, c_EIP150ForkBlock);
    setOptionalU256Parameter(cp.EIP158ForkBlock, c_EIP158ForkBlock);
    setOptionalU256Parameter(cp.byzantiumForkBlock, c_byzantiumForkBlock);
    setOptionalU256Parameter(cp.eWASMForkBlock, c_eWASMForkBlock);
    setOptionalU256Parameter(cp.constantinopleForkBlock, c_constantinopleForkBlock);
    setOptionalU256Parameter(cp.constantinopleFixForkBlock, c_constantinopleFixForkBlock);
    setOptionalU256Parameter(cp.istanbulForkBlock, c_istanbulForkBlock);
    setOptionalU256Parameter(cp.daoHardforkBlock, c_daoHardforkBlock);
    setOptionalU256Parameter(cp.experimentalForkBlock, c_experimentalForkBlock);
    setOptionalU256Parameter(cp.minimumDifficulty, c_minimumDifficulty);
    setOptionalU256Parameter(cp.difficultyBoundDivisor, c_difficultyBoundDivisor);
    setOptionalU256Parameter(cp.durationLimit, c_durationLimit);

    if (params.count(c_chainID))
        cp.chainID = int(fromBigEndian<u256>(fromHex(params.at(c_chainID).get_str())));
    if (params.count(c_networkID))
        cp.networkID = int(fromBigEndian<u256>(fromHex(params.at(c_networkID).get_str())));
    cp.allowFutureBlocks = params.count(c_allowFutureBlocks);

    // genesis
    string genesisStr = js::write_string(obj[c_genesis], false);
    cp = cp.loadGenesis(genesisStr, _stateRoot);
    // genesis state
    string genesisStateStr = js::write_string(obj[c_accounts], false);

    cp.genesisState = jsonToAccountMap(
                genesisStateStr, cp.accountStartNonce, nullptr, &cp.precompiled, _configPath);

    cp.stateRoot = _stateRoot ? _stateRoot : cp.calculateStateRoot(true);
    return cp;
}

ChainParams ChainParams::loadGenesis(string const& _json, h256 const& _stateRoot) const
{
    ChainParams cp(*this);

    js::mValue val;
    js::read_string(_json, val);
    js::mObject genesis = val.get_obj();

    cp.parentHash = h256(0); // required by the YP
    cp.author = genesis.count(c_coinbase) ? h160(genesis[c_coinbase].get_str()) : h160(genesis[c_author].get_str());
    cp.difficulty = genesis.count(c_difficulty) ?
                u256(fromBigEndian<u256>(fromHex(genesis[c_difficulty].get_str()))) :
                cp.minimumDifficulty;
    cp.gasLimit = u256(fromBigEndian<u256>(fromHex(genesis[c_gasLimit].get_str())));
    cp.gasUsed = genesis.count(c_gasUsed) ? u256(fromBigEndian<u256>(fromHex(genesis[c_gasUsed].get_str()))) : 0;
    cp.timestamp = u256(fromBigEndian<u256>(fromHex(genesis[c_timestamp].get_str())));
    cp.extraData = bytes(fromHex(genesis[c_extraData].get_str()));

    // magic code for handling ethash stuff:
    if (genesis.count(c_mixHash) && genesis.count(c_nonce))
    {
        h256 mixHash(genesis[c_mixHash].get_str());
        h64 nonce(genesis[c_nonce].get_str());
        cp.sealFields = 2;
        cp.sealRLP = rlp(mixHash) + rlp(nonce);
    }
    cp.stateRoot = _stateRoot ? _stateRoot : cp.calculateStateRoot();
    return cp;
}

SealEngineFace* ChainParams::createSealEngine()
{
    SealEngineFace* ret = SealEngineRegistrar::create(sealEngineName);
    assert(ret && "Seal engine not found");
    if (!ret)
        return nullptr;
    ret->setChainParams(*this);
    if (sealRLP.empty())
    {
        sealFields = ret->sealFields();
        sealRLP = ret->sealRLP();
    }
    return ret;
}

void ChainParams::populateFromGenesis(bytes const& _genesisRLP, AccountMap const& _state)
{
    BlockHeader bi(_genesisRLP, RLP(&_genesisRLP)[0].isList() ? BlockData : HeaderData);
    parentHash = bi.parentHash();
    author = bi.author();
    difficulty = bi.difficulty();
    gasLimit = bi.gasLimit();
    gasUsed = bi.gasUsed();
    timestamp = bi.timestamp();
    extraData = bi.extraData();
    genesisState = _state;
    RLP r(_genesisRLP);
    sealFields = r[0].itemCount() - BlockHeader::BasicFields;
    sealRLP.clear();
    for (unsigned i = BlockHeader::BasicFields; i < r[0].itemCount(); ++i)
        sealRLP += r[0][i].data();

    calculateStateRoot(true);

    auto b = genesisBlock();
    if (b != _genesisRLP)
    {
        cdebug << "Block passed:" << bi.hash() << bi.hash(WithoutSeal);
        cdebug << "Genesis now:" << BlockHeader::headerHashFromBlock(b);
        cdebug << RLP(b);
        cdebug << RLP(_genesisRLP);
        throw 0;
    }
}

h256 ChainParams::calculateStateRoot(bool _force) const
{
    StateCacheDB db;
    SecureTrieDB<Address, StateCacheDB> state(&db);
    state.init();
    if (!stateRoot || _force)
    {
        // TODO: use hash256
        //stateRoot = hash256(toBytesMap(gs));
        dev::eth::commit(genesisState, state);
        stateRoot = state.root();
    }
    return stateRoot;
}

bytes ChainParams::genesisBlock() const
{
    RLPStream block(3);

    calculateStateRoot();

    block.appendList(BlockHeader::BasicFields + sealFields)
            << parentHash
            << EmptyListSHA3	// sha3(uncles)
            << author
            << stateRoot
            << EmptyTrie		// transactions
            << EmptyTrie		// receipts
            << LogBloom()
            << difficulty
            << 0				// number
            << gasLimit
            << gasUsed			// gasUsed
            << timestamp
            << extraData;
    block.appendRaw(sealRLP, sealFields);
    block.appendRaw(RLPEmptyList);
    block.appendRaw(RLPEmptyList);
    return block.out();
}
