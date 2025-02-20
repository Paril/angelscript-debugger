// MIT Licensed
// see https://github.com/Paril/angelscript-ui-debugger

#define IMGUI_DISABLE_OBSOLETE_FUNCTIONS
#include <angelscript.h>
#include "as_debugger.h"
#include <bitset>

bool asIDBVarView::operator==(const asIDBVarView &other) const
{
    return name == other.name && type == other.type && var == other.var;
}

/*virtual*/ void asIDBCache::Refresh()
{
    CacheCallstack();

    // TODO: this is ugly, we shouldn't need to wipe
    // the whole cache on refreshes. this also makes
    // watch kinda useless.
    locals.clear();
    globals.clear();
    globalsCached = false;
    watch.clear();
    var_states.clear();
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
        const char *section = nullptr;
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
    call_stack.clear();

    if (auto sysfunc = ctx->GetSystemFunction())
        system_function = fmt::format("{} (system function)", sysfunc->GetDeclaration(true, false, true));
    else
        system_function = "";

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
            state.value = evaluators.Evaluate(*this, idKey);
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
            state.value = evaluators.Evaluate(*this, idKey);
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
            state.value = evaluators.Evaluate(*this, idKey);
        }

        map.push_back(asIDBVarView { localName, viewType, stateIt });
    }
}

class asIDBNullTypeEvaluator : public asIDBTypeEvaluator
{
public:
    virtual asIDBVarValue Evaluate(asIDBCache &, asIDBVarAddr id) const override { return { "(null)", true }; }
};

class asIDBUninitTypeEvaluator : public asIDBTypeEvaluator
{
public:
    virtual asIDBVarValue Evaluate(asIDBCache &, asIDBVarAddr id) const override { return { "(uninit)", true }; }
};

class asIDBEnumTypeEvaluator : public asIDBTypeEvaluator
{
public:
    virtual asIDBVarValue Evaluate(asIDBCache &cache, asIDBVarAddr id) const override
    {
        // for enums where we have a single matched value
        // just display it directly
        auto type = cache.ctx->GetEngine()->GetTypeInfoById(id.typeId);
        int v = *reinterpret_cast<const int32_t *>(id.address);

        for (asUINT e = 0; e < type->GetEnumValueCount(); e++)
        {
            int ov = 0;
            const char *name = type->GetEnumValueByIndex(e, &ov);

            if (ov == v)
                return fmt::format("{} ({})", name, v);
        }
        
        std::bitset<32> bits(v);

        if (bits.count() == 1)
            return fmt::format("{}", v );

        return { fmt::format("{} bits", bits.count()), false, asIDBExpandType::Entries };
    }

    virtual void Expand(asIDBCache &cache, asIDBVarAddr id, asIDBVarState &state) const override
    {
        auto type = cache.ctx->GetEngine()->GetTypeInfoById(id.typeId);
        int v = *reinterpret_cast<const int32_t *>(id.address);

        state.entries.push_back({ fmt::format("value: {}", v) });

        for (asUINT e = 0; e < type->GetEnumValueCount(); e++)
        {
            int ov = 0;
            const char *name = type->GetEnumValueByIndex(e, &ov);
            std::bitset<32> obits(ov);

            // skip masks
            if (obits.count() != 1)
                continue;

            if (ov & v)
                state.entries.push_back({ name });
        }
    }
};

class asIDBFuncDefTypeEvaluator : public asIDBTypeEvaluator
{
public:
    virtual asIDBVarValue Evaluate(asIDBCache &, asIDBVarAddr id) const override
    {
        asIScriptFunction *ptr = reinterpret_cast<asIScriptFunction *>(id.address);
        return { ptr->GetName(), false };
    }
};

/*virtual*/ asIDBVarValue asIDBObjectTypeEvaluator::Evaluate(asIDBCache &cache, asIDBVarAddr id) const /*override*/
{
    auto ctx = cache.ctx;
    auto type = ctx->GetEngine()->GetTypeInfoById(id.typeId);
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

/*virtual*/ void asIDBObjectTypeEvaluator::Expand(asIDBCache &cache, asIDBVarAddr id, asIDBVarState &state) const /*override*/
{
    asIScriptObject *obj = nullptr;

    if (id.typeId & asTYPEID_SCRIPTOBJECT)
        obj = (asIScriptObject *) id.address;

    QueryVariableProperties(cache, obj, id, state);

    QueryVariableForEach(cache, id, state);
}

// convenience function that queries the properties of the given
// address (and object, if set) of the given type.
void asIDBObjectTypeEvaluator::QueryVariableProperties(asIDBCache &cache, asIScriptObject *obj, const asIDBVarAddr &id, asIDBVarState &var) const
{
    auto type = cache.ctx->GetEngine()->GetTypeInfoById(id.typeId);

    for (asUINT n = 0; n < (obj ? obj->GetPropertyCount() : type->GetPropertyCount()); n++)
    {
        const char *name;
        int propTypeId;
        void *propAddr = nullptr;
        int offset;
        int compositeOffset;
        bool isCompositeIndirect;

        type->GetProperty(n, &name, &propTypeId, 0, 0, &offset, 0, 0, &compositeOffset, &isCompositeIndirect);

        if (id.typeId & asTYPEID_SCRIPTOBJECT)
            propAddr = obj->GetAddressOfProperty(n);
        else
        {
            // indirect changes our ptr to
            // *(object + compositeOffset) + offset
            if (isCompositeIndirect)
            {
                propAddr = *reinterpret_cast<uint8_t **>(reinterpret_cast<uint8_t *>(id.address) + compositeOffset);

                // if we're null, leave it alone, otherwise point to
                // where we really need to be pointing
                if (propAddr)
                    propAddr = reinterpret_cast<uint8_t *>(propAddr) + offset;
            }
            else
                propAddr = reinterpret_cast<uint8_t *>(id.address) + offset + compositeOffset;
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
        auto state = cache.AddVarState(propId, exists);

        if (exists)
            continue;

        auto &propVar = state->second;

        propVar.value = cache.evaluators.Evaluate(cache, propId);

        var.children.push_back(asIDBVarView { name, cache.GetTypeNameFromType({ propTypeId, asTM_NONE }), state });
    }
}
    
// convenience function that iterates the opFor* of the given
// address (and object, if set) of the given type. If non-zero,
// a specific index will be used.
void asIDBObjectTypeEvaluator::QueryVariableForEach(asIDBCache &cache, const asIDBVarAddr &id, asIDBVarState &var, int index) const
{
    auto ctx = cache.ctx;
    auto type = ctx->GetEngine()->GetTypeInfoById(id.typeId);

    auto opForBegin = type->GetMethodByName("opForBegin");

    if (!opForBegin || opForBegin->GetReturnTypeId() != asTYPEID_UINT32)
        return;

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

    if (index >= 0 && index < opForValues.size())
        opForValues = { opForValues[index] };

    // if we haven't got anything special yet, and we're
    // iterable, we'll show how many elements we have.
    // we'll also just assume the code isn't busted.
    int elementId = 0;

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

        int fv = 0;
        for (auto &opfv : opForValues)
        {
            ctx->Prepare(opfv);
            ctx->SetObject(id.address);
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
            state = cache.AddVarState(elemId, exists);

            if (!exists)
            {
                auto &propVar = state->second;

                if (stackMemory)
                    propVar.stackMemory = std::move(stackMemory);

                propVar.value = cache.evaluators.Evaluate(cache, elemId);
            }

            var.children.push_back(asIDBVarView { fmt::format(opForValues.size() == 1 ? "[{0}]" : "[{0},{1}]", elementId, fv), cache.GetTypeNameFromType({ typeId, asTM_NONE }), state });
            fv++;
        }
                
        ctx->Prepare(opForNext);
        ctx->SetObject(id.address);
        ctx->SetArgDWord(0, rtn);
        ctx->Execute();

        rtn = ctx->GetReturnDWord();

        elementId++;
    }

    ctx->PopState();
}

const asIDBTypeEvaluator &asIDBTypeEvaluatorMap::GetEvaluator(asIDBCache &cache, asIDBVarAddr &id) const
{
    // the only way the base address is null is if
    // it's uninitialized.
    static constexpr const asIDBUninitTypeEvaluator uninitType;

    if (id.address == nullptr)
        return uninitType;

    void *addr = id.address;
    
    // resolve the real address if we're a pointer
    if (id.typeId & (asTYPEID_OBJHANDLE | asTYPEID_HANDLETOCONST))
    {
        static constexpr const asIDBNullTypeEvaluator nullValue;

        addr = *(void **) addr;

        if (addr == nullptr)
            return nullValue;
    }

    id.address = addr;

    // do we have a custom evaluator?
    if (auto f = evaluators.find(id.typeId & (asTYPEID_MASK_OBJECT | asTYPEID_MASK_SEQNBR)); f != evaluators.end())
        return *f->second.get();

    auto type = cache.ctx->GetEngine()->GetTypeInfoById(id.typeId);

    // are we a template?
    if (id.typeId & asTYPEID_TEMPLATE)
    {
        // fetch the base type, see if we have a
        // evaluator for that one
        auto baseType = cache.ctx->GetEngine()->GetTypeInfoByName(type->GetName());

        if (auto f = evaluators.find(baseType->GetTypeId() & (asTYPEID_MASK_OBJECT | asTYPEID_MASK_SEQNBR)); f != evaluators.end())
            return *f->second.get();
    }

    // we'll use the fall back evaluators.
    // check primitives first.
#define CHECK_PRIMITIVE_EVAL(asTypeId, cTypeName) \
    if (id.typeId == asTypeId) \
    { \
        static constexpr const asIDBPrimitiveTypeEvaluator<cTypeName> cTypeName##Type; \
        return cTypeName##Type; \
    }
    
    CHECK_PRIMITIVE_EVAL(asTYPEID_BOOL, bool);
    CHECK_PRIMITIVE_EVAL(asTYPEID_INT8, int8_t);
    CHECK_PRIMITIVE_EVAL(asTYPEID_INT16, int16_t);
    CHECK_PRIMITIVE_EVAL(asTYPEID_INT32, int32_t);
    CHECK_PRIMITIVE_EVAL(asTYPEID_INT64, int64_t);
    CHECK_PRIMITIVE_EVAL(asTYPEID_UINT8, uint8_t);
    CHECK_PRIMITIVE_EVAL(asTYPEID_UINT16, uint16_t);
    CHECK_PRIMITIVE_EVAL(asTYPEID_UINT32, uint32_t);
    CHECK_PRIMITIVE_EVAL(asTYPEID_UINT64, uint64_t);
    CHECK_PRIMITIVE_EVAL(asTYPEID_FLOAT, float);
    CHECK_PRIMITIVE_EVAL(asTYPEID_DOUBLE, double);

#undef CHECK_PRIMITIVE_EVAL

    if (type->GetFlags() & asOBJ_ENUM)
    {
        static constexpr const asIDBEnumTypeEvaluator enumType;
        return enumType;
    }
    else if (type->GetFlags() & asOBJ_FUNCDEF)
    {
        static constexpr const asIDBFuncDefTypeEvaluator funcdefType;
        return funcdefType;
    }

    // finally, just return the base one.
    static constexpr const asIDBObjectTypeEvaluator objectType;
    return objectType;
}

asIDBVarValue asIDBTypeEvaluatorMap::Evaluate(asIDBCache &cache, asIDBVarAddr id) const
{
    return GetEvaluator(cache, id).Evaluate(cache, id);
}

void asIDBTypeEvaluatorMap::Expand(asIDBCache &cache, asIDBVarAddr id, asIDBVarState &state) const
{
    GetEvaluator(cache, id).Expand(cache, id, state);
}

// Register an evaluator.
void asIDBTypeEvaluatorMap::Register(int typeId, std::unique_ptr<asIDBTypeEvaluator> evaluator)
{
    typeId &= asTYPEID_MASK_OBJECT | asTYPEID_MASK_SEQNBR;
    evaluators.insert_or_assign(typeId, std::move(evaluator));
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
        const char *section = nullptr;
        int row = ctx->GetLineNumber(0, nullptr, &section);

        if (section && debugger->breakpoints.find(asIDBBreakpoint::FileLocation(asIDBBreakpointLocation { section, row })) != debugger->breakpoints.end())
            debugger->DebugBreak(ctx);

        // FIXME: this makes an std::string every time
        auto func = ctx->GetFunction(0);
        if (auto f = debugger->breakpoints.find(asIDBBreakpoint::Function(func->GetName())); f != debugger->breakpoints.end())
        {
            debugger->breakpoints.erase(f);
            debugger->DebugBreak(ctx);
        }

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
    cache->Refresh();
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
    asIDBBreakpoint bp = asIDBBreakpoint::FileLocation({ section, line });

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