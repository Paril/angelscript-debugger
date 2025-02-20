// MIT Licensed
// see https://github.com/Paril/angelscript-ui-debugger

#include "as_debugger_imgui.h"
#include "imgui.h"
#include "imgui_internal.h"

void asIDBImGuiFrontend::SetupImGui()
{
    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls

    // Setup Dear ImGui style
    ImGui::StyleColorsDark();

    viewport = ImGui::GetMainViewport();

    SetupImGuiBackend();

    // add default font as fallback for ui
    io.Fonts->AddFontDefault();

    editor.SetReadOnlyEnabled(true);
    editor.SetLanguage(TextEditor::Language::AngelScript());
    editor.SetLineDecorator(17.f, [this](TextEditor::Decorator &decorator) {
        auto size = decorator.height - 1.0f;
        auto pos = ImGui::GetCursorScreenPos();
        auto drawlist = ImGui::GetWindowDrawList();

        if (ImGui::InvisibleButton("##Toggle", ImVec2(size, size)))
            debugger->ToggleBreakpoint(selected_stack_section, decorator.line + 1);

        asIDBBreakpoint bp = asIDBBreakpoint::FileLocation({ selected_stack_section, decorator.line + 1 });

        if (auto it = debugger->breakpoints.find(bp); it != debugger->breakpoints.end())
        {
            drawlist->AddCircleFilled(
                ImVec2(pos.x - 1 + size * 0.5, pos.y + size * 0.5f),
                (size - 6.0f) * 0.5f,
                IM_COL32(255, 0, 0, 255));
        }

        if (decorator.line == update_row - 1)
        {
            float end = size * 0.7;
            const ImVec2 points[] = {
                pos,
                ImVec2(pos.x + end, pos.y),
                ImVec2(pos.x + size, pos.y + size * 0.5f),
                ImVec2(pos.x + end, pos.y + size),
                ImVec2(pos.x, pos.y + size),
                pos
            };
            drawlist->AddPolyline(points, std::extent_v<decltype(points)>,
                (debugger->cache->system_function.empty() && selected_stack_entry == 0) ? IM_COL32(255, 255, 0, 255) : IM_COL32(0, 255, 255, 255),
                ImDrawFlags_RoundCornersAll, 1.5);
        }
    });

    ChangeScript();
}

// script changed, so clear stuff that
// depends on the old script.
void asIDBImGuiFrontend::ChangeScript()
{
    editor.ClearCursors();
    editor.ClearMarkers();

    asIScriptContext *ctx = debugger->cache->ctx;

    auto func = ctx->GetFunction(selected_stack_entry);
    int col;
    const char *sec;
    update_row = ctx->GetLineNumber(selected_stack_entry, &col, &sec);

    if (selected_stack_section != sec)
    {
        selected_stack_section = sec;

        auto file = FetchSource(func->GetModule(), sec);
        editor.SetText(file);
    }

    editor.SetCursor(update_row - 1, 0);
    editor.ScrollToLine(update_row - 1, TextEditor::Scroll::alignMiddle);
    editor.AddMarker(update_row - 1, 0, IM_COL32(127, 127, 0, 127), "", "");

    resetOpenStates = true;
}

// this is the loop for the thread.
// return false if the UI has decided to exit.
bool asIDBImGuiFrontend::Render(bool full)
{
    // check if we need to defer or exit
    {
        asIDBFrameResult result = BackendNewFrame();

        if (result == asIDBFrameResult::Exit)
            return false;
        else if (result == asIDBFrameResult::Defer)
            full = false;
    }

    bool resetText = false;

    ImGui::NewFrame();
    
    dockspace_id = ImGui::DockSpaceOverViewport(0, viewport);

    if (setupDock)
    {
        ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspace_id, viewport->WorkSize);

        {
            ImGuiID dock_id_down = 0, dock_id_top = 0;
            ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Down, 0.20f, &dock_id_down, &dock_id_top);
            ImGui::DockBuilderDockWindow("Call Stack", dock_id_down);
            ImGui::DockBuilderDockWindow("Breakpoints", dock_id_down);

            {
                ImGuiID dock_id_left = 0, dock_id_right = 0;
                ImGui::DockBuilderSplitNode(dock_id_top, ImGuiDir_Left, 0.20f, &dock_id_left, &dock_id_right);
                
                ImGui::DockBuilderDockWindow("Sections", dock_id_left);
                ImGui::DockBuilderDockWindow("Source", dock_id_right);
            }

            {
                ImGuiID dock_id_left = 0, dock_id_right = 0;
                ImGui::DockBuilderSplitNode(dock_id_down, ImGuiDir_Right, 0.5f, &dock_id_right, &dock_id_left);
                ImGui::DockBuilderDockWindow("Parameters", dock_id_right);
                ImGui::DockBuilderDockWindow("Locals", dock_id_right);
                ImGui::DockBuilderDockWindow("Temporaries", dock_id_right);
                ImGui::DockBuilderDockWindow("Globals", dock_id_right);
                ImGui::DockBuilderDockWindow("Watch", dock_id_right);
            }
        }

        ImGui::DockBuilderFinish(dockspace_id);

        setupDock = false;
    }
    
    ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_MenuBar |
        ImGuiWindowFlags_NoBackground;     
    bool show = ImGui::Begin("DockSpace", NULL, windowFlags);

    if (show)
    {
        if (!full)
            ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);

        if (ImGui::BeginMainMenuBar())
        {
            if (ImGui::MenuItem("Continue"))
            {
                debugger->Resume();
            }
            else if (ImGui::MenuItem("Step Into"))
            {
                debugger->StepInto();
            }
            else if (ImGui::MenuItem("Step Over"))
            {
                debugger->StepOver();
            }
            else if (ImGui::MenuItem("Step Out"))
            {
                debugger->StepOut();
            }
            else if (ImGui::MenuItem("Toggle Breakpoint"))
            {
                int line, col;
                editor.GetMainCursor(line, col);
                debugger->ToggleBreakpoint(selected_stack_section, line + 1);
            }
            ImGui::EndMainMenuBar();
        }

        auto &cache = this->debugger->cache;
        auto ctx = cache->ctx;

        if (ImGui::Begin("Call Stack", nullptr, ImGuiWindowFlags_HorizontalScrollbar))
        {
            if (!cache->system_function.empty())
                ImGui::Selectable(cache->system_function.c_str(), false, ImGuiSelectableFlags_Disabled);

            int n = 0;
            for (auto &stack : cache->call_stack)
            {
                bool sel = selected_stack_entry == n;
                if (ImGui::Selectable(stack.declaration.c_str(), &sel))
                {
                    selected_stack_entry = n;
                    resetText = true;
                }

                n++;
            }
        }
        ImGui::End();

        if (!full)
            ImGui::PopItemFlag();

        if (ImGui::Begin("Breakpoints", nullptr, ImGuiWindowFlags_HorizontalScrollbar))
        {
            if (ImGui::BeginTable("##bp", 2,
                ImGuiTableFlags_BordersV | ImGuiTableFlags_BordersOuterH |
                ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg |
                ImGuiTableFlags_NoBordersInBody))
            {
                ImGui::TableSetupColumn("Breakpoint", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Delete", ImGuiTableColumnFlags_WidthFixed);
                ImGui::TableHeadersRow();

                int n = 0;

                for (auto it = debugger->breakpoints.begin(); it != debugger->breakpoints.end(); )
                {
                    auto &bp = *it;
                    ImGui::PushID(n++);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    if (bp.location.index() == 0)
                    {
                        auto &v = std::get<0>(bp.location);
                        ImGui::Text(fmt::format("{} : {}", v.section, v.line).c_str());
                    }
                    else
                        ImGui::Text(std::get<1>(bp.location).c_str());
                    ImGui::TableNextColumn();
                    if (ImGui::Button("X"))
                        it = debugger->breakpoints.erase(it);
                    else
                        it++;
                    ImGui::PopID();
                }

                ImGui::EndTable();
            }
        }
        ImGui::End();

        if (!full)
            ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);

        if (ImGui::Begin("Parameters"))
        {
            ImGui::PushItemWidth(-1);

            static char filterBuf[64] {};
            ImGui::InputText("##Filter", filterBuf, sizeof(filterBuf));
            RenderLocals(filterBuf, asIDBLocalKey(selected_stack_entry, asIDBLocalType::Parameter));
            ImGui::PopItemWidth();
        }
        ImGui::End();
        
        if (ImGui::Begin("Locals"))
        {
            ImGui::PushItemWidth(-1);

            static char filterBuf[64] {};
            ImGui::InputText("##Filter", filterBuf, sizeof(filterBuf));
            RenderLocals(filterBuf, asIDBLocalKey(selected_stack_entry, asIDBLocalType::Variable));
            ImGui::PopItemWidth();
        }
        ImGui::End();
        
        if (ImGui::Begin("Temporaries"))
        {
            ImGui::PushItemWidth(-1);

            static char filterBuf[64] {};
            ImGui::InputText("##Filter", filterBuf, sizeof(filterBuf));
            RenderLocals(filterBuf, asIDBLocalKey(selected_stack_entry, asIDBLocalType::Temporary));
            ImGui::PopItemWidth();
        }
        ImGui::End();
        
        if (ImGui::Begin("Globals"))
        {
            ImGui::PushItemWidth(-1);

            static char filterBuf[64] {};
            ImGui::InputText("##Filter", filterBuf, sizeof(filterBuf));
            RenderGlobals(filterBuf);
            ImGui::PopItemWidth();
        }
        ImGui::End();
        
        if (ImGui::Begin("Watch"))
        {
            ImGui::PushItemWidth(-1);
            RenderWatch();
            ImGui::PopItemWidth();
        }
        ImGui::End();

        if (ImGui::Begin("Sections", nullptr, ImGuiWindowFlags_HorizontalScrollbar))
            for (auto &section : cache->sections)
                ImGui::Selectable(section.second.data(), false);
        ImGui::End();

        if (!full)
            ImGui::PopItemFlag();
        
        if (ImGui::Begin("Source"))
            editor.Render("Source", ImVec2(-1, -1));
        ImGui::End();
    }
    
    ImGui::End();

    // Rendering
    ImGui::EndFrame();

    BackendRender();

    if (resetText)
        ChangeScript();
    
    auto mods = ImGui::GetIO().KeyMods;
    if (full)
    {
        if (ImGui::IsKeyPressed(ImGuiKey::ImGuiKey_F5, false))
            debugger->Resume();
        else if (ImGui::IsKeyPressed(ImGuiKey::ImGuiKey_F10))
            debugger->StepOver();
        else if (ImGui::IsKeyPressed(ImGuiKey::ImGuiKey_F11) && (mods & ImGuiKey::ImGuiMod_Shift) == 0)
            debugger->StepInto();
        else if (ImGui::IsKeyPressed(ImGuiKey::ImGuiKey_F11) && (mods & ImGuiKey::ImGuiMod_Shift) == ImGuiKey::ImGuiMod_Shift)
            debugger->StepOut();
    }

    if (ImGui::IsKeyPressed(ImGuiKey::ImGuiKey_F9, false))
    {
        int line, col;
        editor.GetMainCursor(line, col);
        debugger->ToggleBreakpoint(selected_stack_section, line + 1);
    }

    return true;
}

void asIDBImGuiFrontend::RenderVariableTable(const char *label, const char *filter, asIDBVarViewVector &vars, bool in_watch)
{
    if (ImGui::BeginTable(label, 3,
        ImGuiTableFlags_BordersV | ImGuiTableFlags_BordersOuterH |
        ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_NoBordersInBody))
    {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
            
        for (int n = 0; n < vars.size(); n++)
        {
            ImGui::PushID(n);
            auto global = vars[n];
            RenderDebuggerVariable(global, filter, in_watch);
            ImGui::PopID();
        }

        ImGui::EndTable();
    }
}

void asIDBImGuiFrontend::RenderLocals(const char *filter, asIDBLocalKey stack_entry)
{
    asIDBCache *cache = debugger->cache.get();

    if (auto f = cache->locals.find(stack_entry); f == cache->locals.end())
        cache->CacheLocals(stack_entry);

    auto &f = cache->locals.find(stack_entry)->second;

    RenderVariableTable("##Locals", filter, f, false);
}

void asIDBImGuiFrontend::RenderGlobals(const char *filter)
{
    asIDBCache *cache = debugger->cache.get();

    if (!cache->globalsCached)
        cache->CacheGlobals();

    auto &f = cache->globals;

    RenderVariableTable("##Globals", filter, f, false);
}

void asIDBImGuiFrontend::RenderWatch()
{
    asIDBCache *cache = debugger->cache.get();
    auto &f = cache->watch;

    RenderVariableTable("##Globals", nullptr, f, true);

    if (cache->removeFromWatch != f.end())
    {
        cache->watch.erase(cache->removeFromWatch);
        cache->removeFromWatch = f.end();
    }
}

void asIDBImGuiFrontend::RenderDebuggerVariable(asIDBVarView &varView, const char *filter, bool in_watch)
{
    asIDBCache *cache = debugger->cache.get();
    auto varIt = varView.var;
    auto &var = varIt->second;

    int opened = ImGui::GetStateStorage()->GetInt(ImGui::GetID(varView.name.data(), varView.name.data() + varView.name.size()), 0);
                    
    if (!opened && filter && *filter && varView.name.find(filter) == std::string::npos)
        return;
        
    ImGui::PushID(varView.name.data(), varView.name.data() + varView.name.size());

    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    bool open = ImGui::TreeNodeEx(varView.name.data(), ImGuiTreeNodeFlags_SpanAllColumns | (var.value.expandable == asIDBExpandType::None ? ImGuiTreeNodeFlags_Leaf : ImGuiTreeNodeFlags_None));

    if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
    {
        if (in_watch)
            cache->removeFromWatch = std::find(cache->watch.begin(), cache->watch.end(), varView);

        if (cache->removeFromWatch == cache->watch.end())
        {
            cache->watch.push_back(varView);
            cache->removeFromWatch = cache->watch.end();
        }
    }

    ImGui::TableNextColumn();

    if (open)
    {
        if ((var.value.expandable == asIDBExpandType::Children ||
             var.value.expandable == asIDBExpandType::Entries) && !var.queriedChildren)
        {
            cache->evaluators.Expand(*cache, varIt->first, varIt->second);
            var.queriedChildren = true;
        }
    }

    if (!var.value.value.empty())
    {
        if (var.value.disabled)
            ImGui::BeginDisabled(true);
        auto s = var.value.value.substr(0, 32);
        ImGui::TextUnformatted(s.data(), s.data() + s.size());
        if (var.value.disabled)
            ImGui::EndDisabled();
    }
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(varView.type.data(), varView.type.data() + varView.type.size());

    if (open)
    {
        if (var.value.expandable == asIDBExpandType::Children)
        {
            int i = 0;

            for (auto &child : var.children)
            {
                ImGui::PushID(i);
                RenderDebuggerVariable(child, filter, in_watch);
                ImGui::PopID();

                i++;
            }
        }
        else if (var.value.expandable == asIDBExpandType::Value ||
                 var.value.expandable == asIDBExpandType::Entries)
        {
            // FIXME: how to make this span the entire column?
            // any samples I could find don't deal with long text.
            // I guess we could have a separate "value viewer" tab that
            // can be used if you click a button on an entry or something.
            // Sort of like Watch but specifically for values.

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushTextWrapPos(0.0f);

            if (var.value.expandable == asIDBExpandType::Value)
            {
                const std::string_view s = var.value.value;
                ImGui::TextUnformatted(s.data(), s.data() + s.size());
            }
            else
            {
                for (auto &entry : var.entries)
                {
                    ImGui::Bullet();
                    ImGui::SameLine();
                    ImGui::TextUnformatted(entry.value.data(), entry.value.data() + entry.value.size());
                }
            }

            ImGui::PopTextWrapPos();
        }
        ImGui::TreePop();
    }

    ImGui::PopID();
}