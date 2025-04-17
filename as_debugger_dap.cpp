// MIT Licensed
// see https://github.com/Paril/angelscript-ui-debugger

#include "as_debugger.h"
#include "as_debugger_dap.h"

class asIDBDAPClient
{
public:
    asIDBDebugger                 *dbg;
    std::atomic_bool              configuration_complete = false;
    std::atomic_bool              terminate = false;
    std::unique_ptr<dap::Session> session;

    asIDBDAPClient(asIDBDebugger *dbg, const std::shared_ptr<dap::ReaderWriter> &socket) :
        dbg(dbg),
        session(dap::Session::create())
    {
        session->setOnInvalidData(dap::kClose);
        session->bind(socket);

        session->registerHandler([&](const dap::InitializeRequest &request) {
            dap::InitializeResponse response;
            //response.supportsCompletionsRequest = true;
            response.supportsClipboardContext = true;
            response.supportsConfigurationDoneRequest = true;
            response.supportsDelayedStackTraceLoading = true;
            response.supportsEvaluateForHovers = true;
            response.supportsFunctionBreakpoints = true;
            response.supportsBreakpointLocationsRequest = true;
            return response;
        });

        session->registerHandler([&](const dap::DisconnectRequest &request) { return this->HandleRequest(request); });
        session->registerHandler([&](const dap::SetBreakpointsRequest &request) { return this->HandleRequest(request); });
        session->registerHandler([&](const dap::SetFunctionBreakpointsRequest &request) { return this->HandleRequest(request); });
        session->registerHandler([&](const dap::ConfigurationDoneRequest &request) { return this->HandleRequest(request); });
        session->registerHandler([&](const dap::ThreadsRequest &request) { return this->HandleRequest(request); });
        session->registerHandler([&](const dap::StackTraceRequest &request) { return this->HandleRequest(request); });
        session->registerHandler([&](const dap::ScopesRequest &request) { return this->HandleRequest(request); });
        session->registerHandler([&](const dap::VariablesRequest &request) { return this->HandleRequest(request); });
        session->registerHandler([&](const dap::ContinueRequest &request) { return this->HandleRequest(request); });
        session->registerHandler([&](const dap::StepOutRequest &request) { return this->HandleRequest(request); });
        session->registerHandler([&](const dap::StepInRequest &request) { return this->HandleRequest(request); });
        session->registerHandler([&](const dap::NextRequest &request) { return this->HandleRequest(request); });
        session->registerHandler([&](const dap::EvaluateRequest &request) { return this->HandleRequest(request); });
        session->registerHandler([&](const dap::BreakpointLocationsRequest &request) { return this->HandleRequest(request); });
        
        session->registerSentHandler([&](const dap::ResponseOrError<dap::InitializeResponse> &response) { OnResponseSent(response); });
        session->registerSentHandler([&](const dap::ResponseOrError<dap::ConfigurationDoneResponse> &response) { OnResponseSent(response); });
    }

    dap::BreakpointLocationsResponse HandleRequest(const dap::BreakpointLocationsRequest &request)
    {
        dap::BreakpointLocationsResponse response {};

        if (request.source.name.has_value())
        {
            std::scoped_lock lock(dbg->mutex);
            if (auto linecols = dbg->workspace->potential_breakpoints.find(request.source.name.value()); linecols != dbg->workspace->potential_breakpoints.end())
            {
                // FIXME: STL equal_range maybe?
                for (auto &lc : linecols->second)
                {
                    if (lc.line < request.line)
                        continue;
                    else if (request.endLine.has_value() ?
                        (lc.line > request.endLine.value()) :
                        (lc.line > request.line))
                        continue;
                    else if (request.column.has_value() && lc.col < request.column.value())
                        continue;
                    else if (request.endColumn.has_value() && lc.col > request.endColumn.value())
                        continue;

                    dap::BreakpointLocation bp = response.breakpoints.emplace_back();

                    bp.line = lc.line;
                    bp.column = lc.col;

                    response.breakpoints.push_back(bp);
                }
            }
        }

        return response;
    }

    dap::DisconnectResponse HandleRequest(const dap::DisconnectRequest &request)
    {
        terminate = true;
        return dap::DisconnectResponse {};
    }

    dap::SetBreakpointsResponse HandleRequest(const dap::SetBreakpointsRequest &request)
    {
        dap::SetBreakpointsResponse response;

        if (request.source.path) {
            auto rel = dbg->workspace->PathToSection(request.source.path.value());
            auto pathstr = std::filesystem::path(rel).generic_string();

            std::scoped_lock lock(dbg->mutex);

            dbg->workspace->AddSection(pathstr);

            if (auto it = dbg->breakpoints.find(pathstr); it != dbg->breakpoints.end())
                dbg->breakpoints.erase(it);

            if (request.breakpoints && !request.breakpoints->empty()) {
                auto &pathstrptr = *dbg->workspace->sections.find(pathstr);
                auto &positions = dbg->workspace->potential_breakpoints[pathstrptr];
                auto it = dbg->breakpoints.find(pathstrptr);

                for (auto &bp : *request.breakpoints) {
                    asIDBLineCol closest { -1, -1 };

                    // FIXME: there's probably some STL methods that can
                    // speed up this lookup.
                    for (auto &pos : positions)
                    {
                        if (pos.line != bp.line)
                            continue;
                        else if (bp.column.has_value() && pos.col < bp.column.value())
                            continue;

                        closest.line = pos.line;
                        closest.col = (closest.col == -1) ? pos.col : std::min(closest.col, pos.col);
                    }

                    auto &placed_bp = response.breakpoints.emplace_back();

                    if (closest.line == -1)
                    {
                        placed_bp.verified = false;
                        placed_bp.reason = "failed";
                        placed_bp.message = "No suspend instruction can be found";
                        continue;
                    }

                    placed_bp.line = closest.line;
                    placed_bp.column = closest.col;
                    placed_bp.verified = true;

                    if (it == dbg->breakpoints.end())
                        it = dbg->breakpoints.insert({ pathstrptr, asIDBSectionBreakpoints {} }).first;

                    it->second.push_back({ (int) closest.line, (int) closest.col });
                }
            }
        }

        return response;
    }

    dap::SetFunctionBreakpointsResponse HandleRequest(const dap::SetFunctionBreakpointsRequest &request)
    {
        std::scoped_lock lock(dbg->mutex);
        dap::SetFunctionBreakpointsResponse response {};
        
        dbg->function_breakpoints.clear();

        for (auto &bp : request.breakpoints) {
            dbg->function_breakpoints.insert(bp.name);

            dap::Breakpoint bp {};
            // TODO: validate name
            // TODO: link line/col
            bp.verified = true;
            response.breakpoints.emplace_back(std::move(bp));
        }
        return response;
    }

    dap::ConfigurationDoneResponse HandleRequest(const dap::ConfigurationDoneRequest &request)
    {
        return {};
    }

    dap::ThreadsResponse HandleRequest(const dap::ThreadsRequest &request)
    {
        dap::ThreadsResponse response {};
        response.threads.push_back({
            1, "Main"
        });
        return response;
    }

    dap::StackTraceResponse HandleRequest(const dap::StackTraceRequest &request)
    {
        std::scoped_lock lock(dbg->mutex);
        dap::StackTraceResponse response {};

        if (!dbg->cache || !dbg->cache->ctx)
        {
            response.totalFrames = 1;
            auto &frame = response.stackFrames.emplace_back();
            frame.id = -1;
            frame.presentationHint = "label";
            frame.name = "system func";
            return response;
        }

        dbg->cache->CacheCallstack();

        int64_t start = request.startFrame.has_value() ? (int64_t)(*request.startFrame) : (int64_t)0;
        int64_t levels = (request.levels.has_value() && request.levels.value() > 0) ? (int64_t)(*request.levels) : (int64_t) 9999;

        response.totalFrames = dbg->cache->call_stack.size();
                
        for (asUINT i = 0; i < levels && (size_t) (i + start) < dbg->cache->call_stack.size(); i++)
        {
            auto &frame = response.stackFrames.emplace_back();
            auto &stack = dbg->cache->call_stack[i + start];
            frame.id = stack.id;
            if (stack.scope.offset != SCOPE_SYSTEM)
            {
                frame.line = stack.row;
                frame.column = stack.column;
                dap::Source src;
                src.path = dbg->workspace->SectionToPath(stack.section);
                src.name = std::string(stack.section);
                frame.source = std::move(src);
            }
            else
                frame.presentationHint = "label";
            frame.name = stack.declaration;
        }

        return response;
    }

    dap::ResponseOrError<dap::ScopesResponse> HandleRequest(const dap::ScopesRequest &request)
    {
        std::scoped_lock lock(dbg->mutex);

        dap::ScopesResponse response {};

        if (!dbg->cache)
            return response;

        bool found = false;

        dbg->cache->CacheGlobals();

        for (auto &stack : dbg->cache->call_stack)
        {
            if (stack.id != request.frameId)
                continue;

            if (!stack.scope.locals->Children().empty())
            {
                auto &scope = response.scopes.emplace_back();
                scope.name = "Locals";
                scope.presentationHint = "locals";
                scope.namedVariables = stack.scope.locals->Children().size();
                scope.variablesReference = stack.scope.locals->RefId();
            }
            if (!stack.scope.parameters->Children().empty())
            {
                auto &scope = response.scopes.emplace_back();
                scope.name = "Parameters";
                scope.presentationHint = "parameters";
                scope.namedVariables = stack.scope.parameters->Children().size();
                scope.variablesReference = stack.scope.parameters->RefId();
            }
            if (!stack.scope.registers->Children().empty())
            {
                auto &scope = response.scopes.emplace_back();
                scope.name = "Registers";
                scope.presentationHint = "registers";
                scope.namedVariables = stack.scope.registers->Children().size();
                scope.variablesReference = stack.scope.registers->RefId();
            }
            found = true;
            break;
        }

        if (!found)
            return dap::Error { "invalid stack ID" };

        if (!dbg->cache->globals->Children().empty())
        {
            auto &scope = response.scopes.emplace_back();
            scope.name = "Globals";
            scope.presentationHint = "globals";
            scope.namedVariables = dbg->cache->globals->Children().size();
            scope.expensive = true;
            scope.variablesReference = dbg->cache->globals->RefId();
        }

        return response;
    }

    dap::ResponseOrError<dap::VariablesResponse> HandleRequest(const dap::VariablesRequest &request)
    {
        std::scoped_lock lock(dbg->mutex);
        auto varit = dbg->cache->variable_refs.find(request.variablesReference);

        if (varit == dbg->cache->variable_refs.end())
            return dap::Error("invalid variablesReference");

        auto varContainer = varit->second.lock();
        dap::VariablesResponse response {};

        varContainer->Evaluate();
        varContainer->Expand();

        int64_t start = request.start.has_value() ? (int64_t) request.start.value() : 0;
        int64_t count = request.count.has_value() ? (int64_t) request.count.value() : varContainer->Children().size();

        auto it = varContainer->Children().begin();
        std::advance(it, start);

        for (int64_t i = start; i < count; i++)
        {
            auto &local = *it;
            auto &var = response.variables.emplace_back();

            local->Evaluate();
            var.name = local->identifier.Combine();
            var.type = dap::string(local->typeName);
            if (local->getter)
            {
                dap::VariablePresentationHint hint {};
                hint.lazy = true;
                var.presentationHint = hint;
                var.value = "(...)";
            }
            else
            {
                var.value = local->value.empty() ? local->typeName : local->value;
            }
            var.namedVariables = local->Children().size();
            var.variablesReference = local->RefId();
            it++;
        }

        return response;
    }

    dap::ContinueResponse HandleRequest(const dap::ContinueRequest &request)
    {
        dbg->SetAction(asIDBAction::Continue);
        return {};
    }

    dap::StepOutResponse HandleRequest(const dap::StepOutRequest &request)
    {
        dbg->SetAction(asIDBAction::StepOut);
        return {};
    }

    dap::StepInResponse HandleRequest(const dap::StepInRequest &request)
    {
        dbg->SetAction(asIDBAction::StepInto);
        return {};
    }

    dap::NextResponse HandleRequest(const dap::NextRequest &request)
    {
        dbg->SetAction(asIDBAction::StepOver);
        return {};
    }

    dap::ResponseOrError<dap::EvaluateResponse> HandleRequest(const dap::EvaluateRequest &request)
    {
        std::scoped_lock lock(dbg->mutex);
        dap::EvaluateResponse response {};

        if (!dbg->cache)
            return response;

        std::optional<int> stack_index = std::nullopt;

        if (request.frameId.has_value())
        {
            for (size_t i = 0; i < dbg->cache->call_stack.size(); i++)
                if (request.frameId.value() == dbg->cache->call_stack[i].id)
                {
                    stack_index = i;
                    break;
                }
        }

        auto result = dbg->cache->ResolveExpression(request.expression, stack_index);

        if (!result.has_value())
            return dap::Error { result.error().data() };

        auto var = result.value().lock();

        var->Evaluate();
        
        response.type = dap::string(var->typeName);
        response.result = var->value.empty() ? var->typeName : var->value;
        response.variablesReference = var->RefId();

        return response;
    }

    void OnResponseSent(const dap::ResponseOrError<dap::InitializeResponse> &response)
    {
        {
            std::scoped_lock lock(dbg->mutex);
            dbg->breakpoints.clear();
        }
        session->send(dap::InitializedEvent());
    }

    void OnResponseSent(const dap::ResponseOrError<dap::ConfigurationDoneResponse> &response)
    {
        dap::ThreadEvent threadStartedEvent;
        threadStartedEvent.reason = "started";
        threadStartedEvent.threadId = 1;
        session->send(threadStartedEvent);

        configuration_complete = true;
    }
};

asIDBDAPServer::asIDBDAPServer(int port, asIDBDebugger *debugger) :
    port(port),
    dbg(debugger)
{
}

/*virtual*/ asIDBDAPServer::~asIDBDAPServer()
{
    StopServer();
}

void asIDBDAPServer::ClientConnected(const std::shared_ptr<dap::ReaderWriter> &socket)
{
    client = std::make_unique<asIDBDAPClient>(dbg, socket);
}

void asIDBDAPServer::ClientError(const char *msg)
{
}

void asIDBDAPServer::StartServer()
{
    server = dap::net::Server::create();

    server->start(port,
        [&](const std::shared_ptr<dap::ReaderWriter> &socket) { this->ClientConnected(socket); },
        [&](const char *msg) { this->ClientError(msg); }
    );
}

void asIDBDAPServer::StopServer()
{
    if (client)
        client.reset();
    server.reset();
}

void asIDBDAPServer::Tick()
{
    if (client && client->terminate)
        client.reset();
}

void asIDBDAPServer::SendEventToClient(const dap::TypeInfo *typeinfo, const void *event)
{
    if (client)
        client->session->send(typeinfo, event);
}