// MIT Licensed
// see https://github.com/Paril/angelscript-ui-debugger

#define IMGUI_DISABLE_OBSOLETE_FUNCTIONS
#include <angelscript.h>
#include "as_debugger.h"

bool asIDBVarView::operator==(const asIDBVarView &other) const
{
    return name == other.name && type == other.type && var == other.var;
}

/*virtual*/ const std::string_view asIDBCache::GetTypeNameFromType(asIDBTypeId id)
{
    if (auto f = type_names.find(id); f != type_names.end())
        return f->second.c_str();

    auto type = ctx->GetEngine()->GetTypeInfoById(id.typeId);
    const char *rawName = "???";

    if (!type)
    {
        // a primitive
        switch (id.typeId & asTYPEID_MASK_SEQNBR)
        {
        case asTYPEID_BOOL: rawName = "bool"; break;
        case asTYPEID_INT8: rawName = "int8"; break;
        case asTYPEID_INT16: rawName = "int16"; break;
        case asTYPEID_INT32: rawName = "int32"; break;
        case asTYPEID_INT64: rawName = "int64"; break;
        case asTYPEID_UINT8: rawName = "uint8"; break;
        case asTYPEID_UINT16: rawName = "uint16"; break;
        case asTYPEID_UINT32: rawName = "uint32"; break;
        case asTYPEID_UINT64: rawName = "uint64"; break;
        case asTYPEID_FLOAT: rawName = "float"; break;
        case asTYPEID_DOUBLE: rawName = "double"; break;
        default: rawName = "???"; break;
        }
    }
    else
    {
        rawName = type->GetName();
    }

    std::string name = fmt::format("{}{}{}", (id.modifiers & asTM_CONST) ? "const " : "", rawName,
        ((id.modifiers & asTM_INOUTREF) == asTM_INOUTREF) ? "&" :
        ((id.modifiers & asTM_INOUTREF) == asTM_INREF) ? "&in" :
        ((id.modifiers & asTM_INOUTREF) == asTM_OUTREF) ? "&out" :
        "");

    return type_names.emplace(id, std::move(name)).first->second;
}

/*virtual*/ void asIDBCache::CacheSections()
{
    auto main = ctx->GetFunction(0)->GetModule();

    for (asUINT n = 0; n < main->GetFunctionCount(); n++)
    {
        const char *section;
        main->GetFunctionByIndex(n)->GetDeclaredAt(&section, nullptr, nullptr);
        EnsureSectionCached(section);
    }
}

/*virtual*/ void asIDBCache::EnsureSectionCached(std::string_view section)
{
    sections.insert({ section, section });
}

/*virtual*/ void asIDBCache::CacheCallstack()
{
    if (auto sysfunc = ctx->GetSystemFunction())
        system_function = fmt::format("{} (system function)", sysfunc->GetDeclaration(true, false, true));

    for (asUINT n = 0; n < ctx->GetCallstackSize(); n++)
    {
        auto func = ctx->GetFunction(n);
        int column;
        const char *section;
        int row = ctx->GetLineNumber(n, &column, &section);
        std::string decl = fmt::format("{} Line {}", func->GetDeclaration(true, false, true), row);

        call_stack.push_back(asIDBCallStackEntry {
            std::move(decl),
            section,
            row,
            column
        });

        EnsureSectionCached(call_stack.back().section);
    }
}

/*virtual*/ void asIDBCache::QueryVariableChildren(asIDBVarMap::iterator varIt)
{
    auto &id = varIt->first;
    auto &var = varIt->second;

    var.queriedChildren = true;

    auto type = ctx->GetEngine()->GetTypeInfoById(id.typeId);

    if (!type)
        return;

    asIScriptObject *obj = nullptr;

    void *addr = nullptr;

    if (id.typeId & asTYPEID_SCRIPTOBJECT)
    {
        if (id.typeId & (asTYPEID_OBJHANDLE | asTYPEID_HANDLETOCONST))
            obj = *(asIScriptObject **) id.address;
        else
            obj = (asIScriptObject *) id.address;
    }
    else
    {
        if (id.typeId & (asTYPEID_OBJHANDLE | asTYPEID_HANDLETOCONST))
            addr = *(void **) id.address;
        else
            addr = id.address;
    }

    for (asUINT n = 0; n < (obj ? obj->GetPropertyCount() : type->GetPropertyCount()); n++)
    {
        const char *name;
        int propTypeId;
        void *propAddr = nullptr;
        int offset;

        type->GetProperty(n, &name, &propTypeId, 0, 0, &offset);

        if (id.typeId & asTYPEID_SCRIPTOBJECT)
        {
            propAddr = obj->GetAddressOfProperty(n);
        }
        else
        {
            propAddr = reinterpret_cast<uint8_t *>(addr) + offset;
        }

        asIDBVarAddr propId { propTypeId, propAddr };

        // TODO: variables that overlap memory space will
        // get culled by this. this helps in the case of
        // vec3_t::x and vec3_t::pitch for instance, but
        // causes some confusion for edict_t::number and
        // edict_t::s::number, where `s` is now just an empty
        // struct. it'd be ideal if, in this case, it prefers
        // the deeper nested ones. not sure how we'd express that
        // with the limited context we have, though.
        bool exists;
        auto state = AddVarState(propId, exists);

        if (exists)
            continue;

        auto &propVar = state->second;

        propVar.value = FetchNodeValue(propId);

        var.children.push_back(asIDBVarView { name, GetTypeNameFromType({ propTypeId, asTM_NONE }), state });
    }

    if (auto opForBegin = type->GetMethodByName("opForBegin"))
    {
        if (opForBegin->GetReturnTypeId() != asTYPEID_UINT32)
        {
            return;
        }

        auto opForEnd = type->GetMethodByName("opForEnd");
        auto opForNext = type->GetMethodByName("opForNext");

        auto opForValue = type->GetMethodByName("opForValue");

        std::vector<asIScriptFunction *> opForValues;

        if (!opForValue)
        {
            for (int i = 0; ; i++)
            {
                auto f = type->GetMethodByName(fmt::format("opForValue{}", i).c_str());

                if (!f)
                    break;

                opForValues.push_back(f);
            }
        }
        else
            opForValues.push_back(opForValue);

        // if we haven't got anything special yet, and we're
        // iterable, we'll show how many elements we have.
        // we'll also just assume the code isn't busted.
        int elementId = 0;

        ctx->PushState();
        ctx->Prepare(opForBegin);
        ctx->SetObject(addr);
        ctx->Execute();

        uint32_t rtn = ctx->GetReturnDWord();

        while (true)
        {
            ctx->Prepare(opForEnd);
            ctx->SetObject(addr);
            ctx->SetArgDWord(0, rtn);
            ctx->Execute();
            bool finished = ctx->GetReturnByte();

            if (finished)
                break;

            int fv = 0;
            for (auto &opfv : opForValues)
            {
                ctx->Prepare(opfv);
                ctx->SetObject(addr);
                ctx->SetArgDWord(0, rtn);
                ctx->Execute();

                void *addr = ctx->GetReturnAddress();
                int typeId = opfv->GetReturnTypeId();
                auto type = ctx->GetEngine()->GetTypeInfoById(typeId);
                std::unique_ptr<uint8_t[]> stackMemory;

                // non-heap stuff has to be copied somewhere
                // so the debugger can read it.
                if (!addr)
                {
                    size_t size = type ? type->GetSize() : ctx->GetEngine()->GetSizeOfPrimitiveType(typeId);
                    stackMemory = std::make_unique<uint8_t[]>(size);
                    addr = stackMemory.get();
                    memcpy(addr, ctx->GetAddressOfReturnValue(), size);
                }

                asIDBVarMap::iterator state;

                asIDBVarAddr elemId { typeId, addr };
                bool exists;
                state = AddVarState(elemId, exists);

                if (!exists)
                {
                    auto &propVar = state->second;

                    if (stackMemory)
                        propVar.stackMemory = std::move(stackMemory);

                    propVar.value = FetchNodeValue(elemId);
                }

                var.children.push_back(asIDBVarView { fmt::format("[{},{}]", elementId, fv), GetTypeNameFromType({ typeId, asTM_NONE }), state });
                fv++;
            }
                
            ctx->Prepare(opForNext);
            ctx->SetObject(addr);
            ctx->SetArgDWord(0, rtn);
            ctx->Execute();

            rtn = ctx->GetReturnDWord();

            elementId++;
        }

        ctx->PopState();
    }
}

/*virtual*/ asIDBVarValue asIDBCache::FetchNodeValue(asIDBVarAddr id)
{
    int primitiveId = id.typeId & asTYPEID_MASK_SEQNBR;

    if (primitiveId <= asTYPEID_DOUBLE)
    {
        switch(primitiveId)
        {
        case asTYPEID_VOID: default: return "???";
        case asTYPEID_BOOL: return *reinterpret_cast<const bool *>(id.address) ? "true" : "false";
        case asTYPEID_INT8: return fmt::format("{}", *reinterpret_cast<const int8_t *>(id.address));
        case asTYPEID_INT16: return fmt::format("{}", *reinterpret_cast<const int16_t *>(id.address));
        case asTYPEID_INT32: return fmt::format("{}", *reinterpret_cast<const int32_t *>(id.address));
        case asTYPEID_INT64: return fmt::format("{}", *reinterpret_cast<const int64_t *>(id.address));
        case asTYPEID_UINT8: return fmt::format("{}", *reinterpret_cast<const uint8_t *>(id.address));
        case asTYPEID_UINT16: return fmt::format("{}", *reinterpret_cast<const uint16_t *>(id.address));
        case asTYPEID_UINT32: return fmt::format("{}", *reinterpret_cast<const uint32_t *>(id.address));
        case asTYPEID_UINT64: return fmt::format("{}", *reinterpret_cast<const uint64_t *>(id.address));
        case asTYPEID_FLOAT: return fmt::format("{}", *reinterpret_cast<const float *>(id.address));
        case asTYPEID_DOUBLE: return fmt::format("{}", *reinterpret_cast<const double *>(id.address));
        }
    }

    auto type = ctx->GetEngine()->GetTypeInfoById(id.typeId);

    if (auto func = type->GetFuncdefSignature(); func != nullptr)
    {
        asIScriptFunction *ptr = *reinterpret_cast<asIScriptFunction **>(id.address);

        if (ptr == nullptr)
            return { "null", true };

        return { ptr->GetName(), false };
    }
    else if (type->GetFlags() & asOBJ_ENUM)
    {
        // TODO
        return fmt::format("{}", *reinterpret_cast<const int32_t *>(id.address));
    }
    else if (id.typeId & asTYPEID_OBJHANDLE)
    {
        id.address = reinterpret_cast<uint8_t **>(id.address);

        if (id.address == nullptr)
            return { "null", true };
    }
    else if (!id.address)
    {
        return { "uninit", true };
    }

    bool canExpand = type->GetPropertyCount();
    asIDBVarValue val;

    if (auto opForBegin = type->GetMethodByName("opForBegin"))
    {
        if (opForBegin->GetReturnTypeId() != asTYPEID_UINT32)
        {
            return { "(unsup. iterator)", true, canExpand ? asIDBExpandType::Children : asIDBExpandType::None };
        }

        auto opForEnd = type->GetMethodByName("opForEnd");
        auto opForNext = type->GetMethodByName("opForNext");

        // if we haven't got anything special yet, and we're
        // iterable, we'll show how many elements we have.
        // we'll also just assume the code isn't busted.
        int numElements = 0;

        ctx->PushState();
        ctx->Prepare(opForBegin);
        ctx->SetObject(id.address);
        ctx->Execute();

        uint32_t rtn = ctx->GetReturnDWord();

        while (true)
        {
            ctx->Prepare(opForEnd);
            ctx->SetObject(id.address);
            ctx->SetArgDWord(0, rtn);
            ctx->Execute();
            bool finished = ctx->GetReturnByte();

            if (finished)
                break;
                
            ctx->Prepare(opForNext);
            ctx->SetObject(id.address);
            ctx->SetArgDWord(0, rtn);
            ctx->Execute();

            rtn = ctx->GetReturnDWord();

            numElements++;
        }

        ctx->PopState();

        val.value = fmt::format("{} elements", numElements);
        val.disabled = true;

        if (numElements)
            canExpand = true;
    }

    val.expandable = canExpand ? asIDBExpandType::Children : asIDBExpandType::None;

    return val;
}

/*virtual*/ void asIDBCache::CacheGlobals()
{
    auto main = ctx->GetFunction(0)->GetModule();

    for (asUINT n = 0; n < main->GetGlobalVarCount(); n++)
    {
        const char *name;
        int typeId;
        void *ptr;
        bool isConst;

        main->GetGlobalVar(n, &name, nullptr, &typeId, &isConst);
        ptr = main->GetAddressOfGlobalVar(n);

        asIDBTypeId typeKey { typeId, isConst ? asTM_CONST : asTM_NONE };
        const std::string_view viewType = GetTypeNameFromType(typeKey);

        asIDBVarAddr idKey { typeId, ptr };

        // globals can safely appear in more than one spot
        bool exists;
        auto stateIt = AddVarState(idKey, exists);

        if (!exists)
        {
            auto &state = stateIt->second;
            state.value = FetchNodeValue(idKey);
        }

        globals.push_back(asIDBVarView { name, viewType, stateIt });
    }

    globalsCached = true;
}

/*virtual*/ void asIDBCache::CacheLocals(asIDBLocalKey stack_entry)
{
    // variables in AS are always ordered the same way it seems:
    // function parameters come first,
    // then local variables,
    // then temporaries used during calculations.
    int numLocals = ctx->GetVarCount(stack_entry.offset);
    int numParams = ctx->GetFunction(stack_entry.offset)->GetParamCount();
    int numTemporaries = 0;
        
    for (int s = numParams; s < numLocals; s++)
    {
        const char *name;
        ctx->GetVar(s, stack_entry.offset, &name);

        if (!name || !*name)
        {
            numTemporaries = numLocals - s;
            break;
        }
    }

    int numVariables = numLocals - numParams - numTemporaries;
    int start = 0, end = 0;

    if (stack_entry.type == asIDBLocalType::Parameter)
        end = numParams;
    else if (stack_entry.type == asIDBLocalType::Variable)
    {
        start = numParams;
        end = numParams + numVariables;
    }
    else
    {
        start = numParams + numVariables;
        end = numLocals;
    }

    auto &map = locals[stack_entry];

    if (auto thisPtr = ctx->GetThisPointer(stack_entry.offset))
    {
        int thisTypeId = ctx->GetThisTypeId(stack_entry.offset);

        asIDBTypeId typeKey { thisTypeId, asTM_NONE };

        const std::string_view viewType = GetTypeNameFromType(typeKey);

        asIDBVarAddr idKey { thisTypeId, thisPtr };

        // locals can safely appear in more than one spot
        bool exists;
        auto stateIt = AddVarState(idKey, exists);

        if (!exists)
        {
            auto &state = stateIt->second;
            state.value = FetchNodeValue(idKey);
        }

        map.push_back(asIDBVarView { "this", viewType, stateIt });
    }

    for (int n = start; n < end; n++)
    {
        const char *name;
        int typeId;
        asETypeModifiers modifiers;
        int stackOffset;
        ctx->GetVar(n, stack_entry.offset, &name, &typeId, &modifiers, nullptr, &stackOffset);
        void *ptr = ctx->GetAddressOfVar(n, stack_entry.offset);

        asIDBTypeId typeKey { typeId, modifiers };

        std::string localName = (name && *name) ? name : fmt::format("& {}", stackOffset);

        const std::string_view viewType = GetTypeNameFromType(typeKey);

        asIDBVarAddr idKey { typeId, ptr };

        // locals can safely appear in more than one spot
        bool exists;
        auto stateIt = AddVarState(idKey, exists);

        if (!exists)
        {
            auto &state = stateIt->second;
            state.value = FetchNodeValue(idKey);
        }

        map.push_back(asIDBVarView { localName, viewType, stateIt });
    }
}

/*static*/ void asIDBDebugger::LineCallback(asIScriptContext *ctx, asIDBDebugger *debugger)
{
    // we might not have an action - functions called from within
    // the debugger will never have this set.
    if (debugger->action != asIDBAction::None)
    {
        // Step Into just breaks on whatever happens to be next.
        if (debugger->action == asIDBAction::StepInto)
        {
            debugger->DebugBreak(ctx);
            return;
        }
        // Step Over breaks on the next line that is <= the
        // current stack level.
        else if (debugger->action == asIDBAction::StepOver)
        {
            if (ctx->GetCallstackSize() <= debugger->stack_size)
                debugger->DebugBreak(ctx);
            return;
        }
        // Step Out breaks on the next line that is < the
        // current stack level.
        else if (debugger->action == asIDBAction::StepOut)
        {
            if (ctx->GetCallstackSize() < debugger->stack_size)
                debugger->DebugBreak(ctx);
            return;
        }
    }

    // breakpoints are handled here. note that a single
    // breakpoint can be hit by multiple things on the same
    // line.
    if (!debugger->breakpoints.empty())
    {
        const char *section;
        int row = ctx->GetLineNumber(0, nullptr, &section);

        if (section && debugger->breakpoints.find(asIDBBreakpoint { section, row }) != debugger->breakpoints.end())
            debugger->DebugBreak(ctx);

        return;
    }
}

void asIDBDebugger::HookContext(asIScriptContext *ctx)
{
    if (cache && cache->ctx == ctx)
        return;
    
    cache = CreateCache(ctx);
    ctx->SetLineCallback(asFUNCTION(asIDBDebugger::LineCallback), this, asCALL_CDECL);
}

void asIDBDebugger::DebugBreak(asIScriptContext *ctx)
{
    action = asIDBAction::None;
    HookContext(ctx);
    Suspend();
}

bool asIDBDebugger::HasWork()
{
    return !breakpoints.empty();
}

// debugger operations; these set the next breakpoint
// and call Resume.
void asIDBDebugger::StepInto()
{
    action = asIDBAction::StepInto;
    stack_size = cache->ctx->GetCallstackSize();
    Resume();
}

void asIDBDebugger::StepOver()
{
    action = asIDBAction::StepOver;
    stack_size = cache->ctx->GetCallstackSize();
    Resume();
}

void asIDBDebugger::StepOut()
{
    action = asIDBAction::StepOut;
    stack_size = cache->ctx->GetCallstackSize();
    Resume();
}

bool asIDBDebugger::ToggleBreakpoint(std::string_view section, int line)
{
    asIDBBreakpoint bp { section, line };

    if (auto f = breakpoints.find(bp); f != breakpoints.end())
    {
        breakpoints.erase(f);
        return false;
    }
    else
    {
        breakpoints.insert(bp);
        return true;
    }
}