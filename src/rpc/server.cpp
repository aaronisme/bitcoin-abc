// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2018 The Bitcoin Core developers
// Copyright (c) 2018-2019 The Bitcoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <rpc/server.h>

#include <config.h>
#include <fs.h>
#include <key_io.h>
#include <random.h>
#include <rpc/util.h>
#include <shutdown.h>
#include <sync.h>
#include <ui_interface.h>
#include <util/strencodings.h>
#include <util/system.h>

#include <univalue.h>

#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/signals2/signal.hpp>

#include <memory> // for unique_ptr
#include <set>
#include <unordered_map>

static std::atomic<bool> g_rpc_running{false};
static bool fRPCInWarmup = true;
static std::string rpcWarmupStatus("RPC server started");
static CCriticalSection cs_rpcWarmup;
/* Timer-creating functions */
static RPCTimerInterface *timerInterface = nullptr;
/* Map of name to timer. */
static std::map<std::string, std::unique_ptr<RPCTimerBase>> deadlineTimers;

struct RPCCommandExecutionInfo {
    std::string method;
    int64_t start;
};

struct RPCServerInfo {
    Mutex mutex;
    std::list<RPCCommandExecutionInfo> active_commands GUARDED_BY(mutex);
};

static RPCServerInfo g_rpc_server_info;

struct RPCCommandExecution {
    std::list<RPCCommandExecutionInfo>::iterator it;
    explicit RPCCommandExecution(const std::string &method) {
        LOCK(g_rpc_server_info.mutex);
        it = g_rpc_server_info.active_commands.insert(
            g_rpc_server_info.active_commands.cend(),
            {method, GetTimeMicros()});
    }
    ~RPCCommandExecution() {
        LOCK(g_rpc_server_info.mutex);
        g_rpc_server_info.active_commands.erase(it);
    }
};

UniValue RPCServer::ExecuteCommand(Config &config,
                                   const JSONRPCRequest &request) const {
    // Return immediately if in warmup
    // This is retained from the old RPC implementation because a lot of state
    // is set during warmup that RPC commands may depend on.  This can be
    // safely removed once global variable usage has been eliminated.
    {
        LOCK(cs_rpcWarmup);
        if (fRPCInWarmup) {
            throw JSONRPCError(RPC_IN_WARMUP, rpcWarmupStatus);
        }
    }

    std::string commandName = request.strMethod;
    {
        auto commandsReadView = commands.getReadView();
        auto iter = commandsReadView->find(commandName);
        if (iter != commandsReadView.end()) {
            return iter->second.get()->Execute(request);
        }
    }

    // TODO Remove the below call to tableRPC.execute() and only call it for
    // context-free RPC commands via an implementation of RPCCommand.

    // Check if context-free RPC method is valid and execute it
    return tableRPC.execute(config, request);
}

void RPCServer::RegisterCommand(std::unique_ptr<RPCCommand> command) {
    if (command != nullptr) {
        const std::string &commandName = command->GetName();
        commands.getWriteView()->insert(
            std::make_pair(commandName, std::move(command)));
    }
}

static struct CRPCSignals {
    boost::signals2::signal<void()> Started;
    boost::signals2::signal<void()> Stopped;
} g_rpcSignals;

void RPCServerSignals::OnStarted(std::function<void()> slot) {
    g_rpcSignals.Started.connect(slot);
}

void RPCServerSignals::OnStopped(std::function<void()> slot) {
    g_rpcSignals.Stopped.connect(slot);
}

void RPCTypeCheck(const UniValue &params,
                  const std::list<UniValueType> &typesExpected,
                  bool fAllowNull) {
    unsigned int i = 0;
    for (const UniValueType &t : typesExpected) {
        if (params.size() <= i) {
            break;
        }

        const UniValue &v = params[i];
        if (!(fAllowNull && v.isNull())) {
            RPCTypeCheckArgument(v, t);
        }
        i++;
    }
}

void RPCTypeCheckArgument(const UniValue &value,
                          const UniValueType &typeExpected) {
    if (!typeExpected.typeAny && value.type() != typeExpected.type) {
        throw JSONRPCError(RPC_TYPE_ERROR,
                           strprintf("Expected type %s, got %s",
                                     uvTypeName(typeExpected.type),
                                     uvTypeName(value.type())));
    }
}

void RPCTypeCheckObj(const UniValue &o,
                     const std::map<std::string, UniValueType> &typesExpected,
                     bool fAllowNull, bool fStrict) {
    for (const auto &t : typesExpected) {
        const UniValue &v = find_value(o, t.first);
        if (!fAllowNull && v.isNull()) {
            throw JSONRPCError(RPC_TYPE_ERROR,
                               strprintf("Missing %s", t.first));
        }

        if (!(t.second.typeAny || v.type() == t.second.type ||
              (fAllowNull && v.isNull()))) {
            std::string err = strprintf("Expected type %s for %s, got %s",
                                        uvTypeName(t.second.type), t.first,
                                        uvTypeName(v.type()));
            throw JSONRPCError(RPC_TYPE_ERROR, err);
        }
    }

    if (fStrict) {
        for (const std::string &k : o.getKeys()) {
            if (typesExpected.count(k) == 0) {
                std::string err = strprintf("Unexpected key %s", k);
                throw JSONRPCError(RPC_TYPE_ERROR, err);
            }
        }
    }
}

Amount AmountFromValue(const UniValue &value) {
    if (!value.isNum() && !value.isStr()) {
        throw JSONRPCError(RPC_TYPE_ERROR, "Amount is not a number or string");
    }

    int64_t n;
    if (!ParseFixedPoint(value.getValStr(), 8, &n)) {
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid amount");
    }

    Amount amt = n * SATOSHI;
    if (!MoneyRange(amt)) {
        throw JSONRPCError(RPC_TYPE_ERROR, "Amount out of range");
    }

    return amt;
}

uint256 ParseHashV(const UniValue &v, std::string strName) {
    std::string strHex(v.get_str());
    if (64 != strHex.length()) {
        throw JSONRPCError(
            RPC_INVALID_PARAMETER,
            strprintf("%s must be of length %d (not %d, for '%s')", strName, 64,
                      strHex.length(), strHex));
    }
    // Note: IsHex("") is false
    if (!IsHex(strHex)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER,
                           strName + " must be hexadecimal string (not '" +
                               strHex + "')");
    }
    return uint256S(strHex);
}
uint256 ParseHashO(const UniValue &o, std::string strKey) {
    return ParseHashV(find_value(o, strKey), strKey);
}
std::vector<uint8_t> ParseHexV(const UniValue &v, std::string strName) {
    std::string strHex;
    if (v.isStr()) {
        strHex = v.get_str();
    }
    if (!IsHex(strHex)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER,
                           strName + " must be hexadecimal string (not '" +
                               strHex + "')");
    }

    return ParseHex(strHex);
}
std::vector<uint8_t> ParseHexO(const UniValue &o, std::string strKey) {
    return ParseHexV(find_value(o, strKey), strKey);
}

/**
 * Note: This interface may still be subject to change.
 */
std::string CRPCTable::help(Config &config, const std::string &strCommand,
                            const JSONRPCRequest &helpreq) const {
    std::string strRet;
    std::string category;
    std::set<const ContextFreeRPCCommand *> setDone;
    std::vector<std::pair<std::string, const ContextFreeRPCCommand *>>
        vCommands;

    for (const auto &entry : mapCommands) {
        vCommands.push_back(
            std::make_pair(entry.second->category + entry.first, entry.second));
    }
    sort(vCommands.begin(), vCommands.end());

    JSONRPCRequest jreq(helpreq);
    jreq.fHelp = true;
    jreq.params = UniValue();

    for (const std::pair<std::string, const ContextFreeRPCCommand *> &command :
         vCommands) {
        const ContextFreeRPCCommand *pcmd = command.second;
        std::string strMethod = pcmd->name;
        if ((strCommand != "" || pcmd->category == "hidden") &&
            strMethod != strCommand) {
            continue;
        }

        jreq.strMethod = strMethod;
        try {
            if (setDone.insert(pcmd).second) {
                pcmd->call(config, jreq);
            }
        } catch (const std::exception &e) {
            // Help text is returned in an exception
            std::string strHelp = std::string(e.what());
            if (strCommand == "") {
                if (strHelp.find('\n') != std::string::npos) {
                    strHelp = strHelp.substr(0, strHelp.find('\n'));
                }

                if (category != pcmd->category) {
                    if (!category.empty()) {
                        strRet += "\n";
                    }
                    category = pcmd->category;
                    strRet += "== " + Capitalize(category) + " ==\n";
                }
            }
            strRet += strHelp + "\n";
        }
    }
    if (strRet == "") {
        strRet = strprintf("help: unknown command: %s\n", strCommand);
    }

    strRet = strRet.substr(0, strRet.size() - 1);
    return strRet;
}

static UniValue help(Config &config, const JSONRPCRequest &jsonRequest) {
    if (jsonRequest.fHelp || jsonRequest.params.size() > 1) {
        throw std::runtime_error(
            RPCHelpMan{
                "help",
                "\nList all commands, or get help for a specified command.\n",
                {
                    {"command", RPCArg::Type::STR, true},
                }}
                .ToString() +
            "\nArguments:\n"
            "1. \"command\"     (string, optional) The command to get help on\n"
            "\nResult:\n"
            "\"text\"     (string) The help text\n");
    }

    std::string strCommand;
    if (jsonRequest.params.size() > 0) {
        strCommand = jsonRequest.params[0].get_str();
    }

    return tableRPC.help(config, strCommand, jsonRequest);
}

static UniValue stop(const Config &config, const JSONRPCRequest &jsonRequest) {
    // Accept the deprecated and ignored 'detach' boolean argument
    // Also accept the hidden 'wait' integer argument (milliseconds)
    // For instance, 'stop 1000' makes the call wait 1 second before returning
    // to the client (intended for testing)
    if (jsonRequest.fHelp || jsonRequest.params.size() > 1) {
        throw std::runtime_error(
            RPCHelpMan{"stop", "\nStop Bitcoin server.", {}}.ToString());
    }

    // Event loop will exit after current HTTP requests have been handled, so
    // this reply will get back to the client.
    StartShutdown();
    if (jsonRequest.params[0].isNum()) {
        MilliSleep(jsonRequest.params[0].get_int());
    }
    return "Bitcoin server stopping";
}

static UniValue uptime(const Config &config,
                       const JSONRPCRequest &jsonRequest) {
    if (jsonRequest.fHelp || jsonRequest.params.size() > 0) {
        throw std::runtime_error(
            RPCHelpMan{
                "uptime", "\nReturns the total uptime of the server.\n", {}}
                .ToString() +
            "\nResult:\n"
            "ttt        (numeric) The number of seconds "
            "that the server has been running\n"
            "\nExamples:\n" +
            HelpExampleCli("uptime", "") + HelpExampleRpc("uptime", ""));
    }

    return GetTime() - GetStartupTime();
}

static UniValue getrpcinfo(const Config &config,
                           const JSONRPCRequest &request) {
    if (request.fHelp || request.params.size() > 0) {
        throw std::runtime_error(RPCHelpMan{
            "getrpcinfo",
            "\nReturns details of the RPC server.\n",
            {}}.ToString());
    }

    LOCK(g_rpc_server_info.mutex);
    UniValue active_commands(UniValue::VARR);
    for (const RPCCommandExecutionInfo &info :
         g_rpc_server_info.active_commands) {
        UniValue entry(UniValue::VOBJ);
        entry.pushKV("method", info.method);
        entry.pushKV("duration", GetTimeMicros() - info.start);
        active_commands.push_back(entry);
    }

    UniValue result(UniValue::VOBJ);
    result.pushKV("active_commands", active_commands);

    return result;
}

/**
 * Call Table
 */
// clang-format off
static const ContextFreeRPCCommand vRPCCommands[] = {
    //  category            name                      actor (function)        argNames
    //  ------------------- ------------------------  ----------------------  ----------
    /* Overall control/query calls */
    { "control",            "getrpcinfo",             getrpcinfo,             {}  },
    { "control",            "help",                   help,                   {"command"}  },
    { "control",            "stop",                   stop,                   {"wait"}  },
    { "control",            "uptime",                 uptime,                 {}  },
};
// clang-format on

CRPCTable::CRPCTable() {
    unsigned int vcidx;
    for (vcidx = 0; vcidx < (sizeof(vRPCCommands) / sizeof(vRPCCommands[0]));
         vcidx++) {
        const ContextFreeRPCCommand *pcmd;

        pcmd = &vRPCCommands[vcidx];
        mapCommands[pcmd->name] = pcmd;
    }
}

const ContextFreeRPCCommand *CRPCTable::
operator[](const std::string &name) const {
    std::map<std::string, const ContextFreeRPCCommand *>::const_iterator it =
        mapCommands.find(name);
    if (it == mapCommands.end()) {
        return nullptr;
    }

    return (*it).second;
}

bool CRPCTable::appendCommand(const std::string &name,
                              const ContextFreeRPCCommand *pcmd) {
    if (IsRPCRunning()) {
        return false;
    }

    // don't allow overwriting for now
    std::map<std::string, const ContextFreeRPCCommand *>::const_iterator it =
        mapCommands.find(name);
    if (it != mapCommands.end()) {
        return false;
    }

    mapCommands[name] = pcmd;
    return true;
}

void StartRPC() {
    LogPrint(BCLog::RPC, "Starting RPC\n");
    g_rpc_running = true;
    g_rpcSignals.Started();
}

void InterruptRPC() {
    LogPrint(BCLog::RPC, "Interrupting RPC\n");
    // Interrupt e.g. running longpolls
    g_rpc_running = false;
}

void StopRPC() {
    LogPrint(BCLog::RPC, "Stopping RPC\n");
    deadlineTimers.clear();
    DeleteAuthCookie();
    g_rpcSignals.Stopped();
}

bool IsRPCRunning() {
    return g_rpc_running;
}

void SetRPCWarmupStatus(const std::string &newStatus) {
    LOCK(cs_rpcWarmup);
    rpcWarmupStatus = newStatus;
}

void SetRPCWarmupFinished() {
    LOCK(cs_rpcWarmup);
    assert(fRPCInWarmup);
    fRPCInWarmup = false;
}

bool RPCIsInWarmup(std::string *outStatus) {
    LOCK(cs_rpcWarmup);
    if (outStatus) {
        *outStatus = rpcWarmupStatus;
    }
    return fRPCInWarmup;
}

bool IsDeprecatedRPCEnabled(ArgsManager &args, const std::string &method) {
    const std::vector<std::string> enabled_methods =
        args.GetArgs("-deprecatedrpc");

    return find(enabled_methods.begin(), enabled_methods.end(), method) !=
           enabled_methods.end();
}

static UniValue JSONRPCExecOne(Config &config, RPCServer &rpcServer,
                               JSONRPCRequest jreq, const UniValue &req) {
    UniValue rpc_result(UniValue::VOBJ);

    try {
        jreq.parse(req);

        UniValue result = rpcServer.ExecuteCommand(config, jreq);
        rpc_result = JSONRPCReplyObj(result, NullUniValue, jreq.id);
    } catch (const UniValue &objError) {
        rpc_result = JSONRPCReplyObj(NullUniValue, objError, jreq.id);
    } catch (const std::exception &e) {
        rpc_result = JSONRPCReplyObj(
            NullUniValue, JSONRPCError(RPC_PARSE_ERROR, e.what()), jreq.id);
    }

    return rpc_result;
}

std::string JSONRPCExecBatch(Config &config, RPCServer &rpcServer,
                             const JSONRPCRequest &jreq, const UniValue &vReq) {
    UniValue ret(UniValue::VARR);
    for (size_t i = 0; i < vReq.size(); i++) {
        ret.push_back(JSONRPCExecOne(config, rpcServer, jreq, vReq[i]));
    }

    return ret.write() + "\n";
}

/**
 * Process named arguments into a vector of positional arguments, based on the
 * passed-in specification for the RPC call's arguments.
 */
static inline JSONRPCRequest
transformNamedArguments(const JSONRPCRequest &in,
                        const std::vector<std::string> &argNames) {
    JSONRPCRequest out = in;
    out.params = UniValue(UniValue::VARR);
    // Build a map of parameters, and remove ones that have been processed, so
    // that we can throw a focused error if there is an unknown one.
    const std::vector<std::string> &keys = in.params.getKeys();
    const std::vector<UniValue> &values = in.params.getValues();
    std::unordered_map<std::string, const UniValue *> argsIn;
    for (size_t i = 0; i < keys.size(); ++i) {
        argsIn[keys[i]] = &values[i];
    }
    // Process expected parameters.
    int hole = 0;
    for (const std::string &argNamePattern : argNames) {
        std::vector<std::string> vargNames;
        boost::algorithm::split(vargNames, argNamePattern,
                                boost::algorithm::is_any_of("|"));
        auto fr = argsIn.end();
        for (const std::string &argName : vargNames) {
            fr = argsIn.find(argName);
            if (fr != argsIn.end()) {
                break;
            }
        }
        if (fr != argsIn.end()) {
            for (int i = 0; i < hole; ++i) {
                // Fill hole between specified parameters with JSON nulls, but
                // not at the end (for backwards compatibility with calls that
                // act based on number of specified parameters).
                out.params.push_back(UniValue());
            }
            hole = 0;
            out.params.push_back(*fr->second);
            argsIn.erase(fr);
        } else {
            hole += 1;
        }
    }
    // If there are still arguments in the argsIn map, this is an error.
    if (!argsIn.empty()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER,
                           "Unknown named parameter " + argsIn.begin()->first);
    }
    // Return request with named arguments transformed to positional arguments
    return out;
}

UniValue CRPCTable::execute(Config &config,
                            const JSONRPCRequest &request) const {
    // Return immediately if in warmup
    {
        LOCK(cs_rpcWarmup);
        if (fRPCInWarmup) {
            throw JSONRPCError(RPC_IN_WARMUP, rpcWarmupStatus);
        }
    }

    // Check if legacy RPC method is valid.
    // See RPCServer::ExecuteCommand for context-sensitive RPC commands.
    const ContextFreeRPCCommand *pcmd = tableRPC[request.strMethod];
    if (!pcmd) {
        throw JSONRPCError(RPC_METHOD_NOT_FOUND, "Method not found");
    }

    try {
        RPCCommandExecution execution(request.strMethod);
        // Execute, convert arguments to array if necessary
        if (request.params.isObject()) {
            return pcmd->call(config,
                              transformNamedArguments(request, pcmd->argNames));
        } else {
            return pcmd->call(config, request);
        }
    } catch (const std::exception &e) {
        throw JSONRPCError(RPC_MISC_ERROR, e.what());
    }
}

std::vector<std::string> CRPCTable::listCommands() const {
    std::vector<std::string> commandList;
    for (const auto &i : mapCommands) {
        commandList.emplace_back(i.first);
    }
    return commandList;
}

std::string HelpExampleCli(const std::string &methodname,
                           const std::string &args) {
    return "> bitcoin-cli " + methodname + " " + args + "\n";
}

std::string HelpExampleRpc(const std::string &methodname,
                           const std::string &args) {
    return "> curl --user myusername --data-binary '{\"jsonrpc\": \"1.0\", "
           "\"id\":\"curltest\", "
           "\"method\": \"" +
           methodname + "\", \"params\": [" + args +
           "] }' -H 'content-type: text/plain;' http://127.0.0.1:8332/\n";
}

void RPCSetTimerInterfaceIfUnset(RPCTimerInterface *iface) {
    if (!timerInterface) {
        timerInterface = iface;
    }
}

void RPCSetTimerInterface(RPCTimerInterface *iface) {
    timerInterface = iface;
}

void RPCUnsetTimerInterface(RPCTimerInterface *iface) {
    if (timerInterface == iface) {
        timerInterface = nullptr;
    }
}

void RPCRunLater(const std::string &name, std::function<void()> func,
                 int64_t nSeconds) {
    if (!timerInterface) {
        throw JSONRPCError(RPC_INTERNAL_ERROR,
                           "No timer handler registered for RPC");
    }
    deadlineTimers.erase(name);
    LogPrint(BCLog::RPC, "queue run of timer %s in %i seconds (using %s)\n",
             name, nSeconds, timerInterface->Name());
    deadlineTimers.emplace(
        name, std::unique_ptr<RPCTimerBase>(
                  timerInterface->NewTimer(func, nSeconds * 1000)));
}

int RPCSerializationFlags() {
    return 0;
}

CRPCTable tableRPC;
