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

    ChangeScript();
}

// script changed, so clear stuff that
// depends on the old script.
void asIDBImGuiFrontend::ChangeScript()
{
    editor.ClearCursors();
    editor.ClearErrorMarkers();

    asIScriptContext *ctx = debugger->cache->ctx;

    auto func = ctx->GetFunction(selected_stack_entry);
    int col;
    const char *sec;
    update_row = ctx->GetLineNumber(selected_stack_entry, &col, &sec);

    selected_stack_section = sec;

    auto file = FetchSource(func->GetModule(), sec);
    editor.SetText(file);
    editor.AddErrorMarker(update_row - 1, "Stack Entry");

    editor.SetArrow(update_row - 1, (debugger->cache->system_function.empty() && selected_stack_entry == 0) ?
        TextEditor::LineArrow::statement : TextEditor::LineArrow::returnStatement);

    for (auto &bp : debugger->breakpoints)
    {
        if (bp.location.index() == 0)
        {
            auto &loc = std::get<0>(bp.location);
            if (loc.section == selected_stack_section)
                editor.SetBreakpoint(loc.line - 1, true);
        }
    }

    update_cursor = 2;
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
                editor.SetBreakpoint(editor.GetCursorLine(), debugger->ToggleBreakpoint(selected_stack_section, editor.GetCursorLine() + 1));
            }
            ImGui::EndMainMenuBar();
        }

        auto &cache = this->debugger->cache;
        auto ctx = cache->ctx;

        if (ImGui::Begin("Call Stack", nullptr, ImGuiWindowFlags_HorizontalScrollbar))
        {
            if (full)
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
        }
        ImGui::End();

        if (ImGui::Begin("Breakpoints", nullptr, ImGuiWindowFlags_HorizontalScrollbar))
        {
            if (full)
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
                        {
                            if (bp.location.index() == 0)
                            {
                                auto &v = std::get<0>(bp.location);

                                if (selected_stack_section == v.section)
                                    editor.SetBreakpoint(v.line - 1, false);
                            }
                            it = debugger->breakpoints.erase(it);
                        }
                        else
                            it++;
                        ImGui::PopID();
                    }

                    ImGui::EndTable();
                }
            }
        }
        ImGui::End();

        if (ImGui::Begin("Parameters"))
        {
            if (full)
            {
                ImGui::PushItemWidth(-1);

                static char filterBuf[64] {};
                ImGui::InputText("##Filter", filterBuf, sizeof(filterBuf));
                RenderLocals(filterBuf, asIDBLocalKey(selected_stack_entry, asIDBLocalType::Parameter));
                ImGui::PopItemWidth();
            }
        }
        ImGui::End();
        
        if (ImGui::Begin("Locals"))
        {
            if (full)
            {
                ImGui::PushItemWidth(-1);

                static char filterBuf[64] {};
                ImGui::InputText("##Filter", filterBuf, sizeof(filterBuf));
                RenderLocals(filterBuf, asIDBLocalKey(selected_stack_entry, asIDBLocalType::Variable));
                ImGui::PopItemWidth();
            }
        }
        ImGui::End();
        
        if (ImGui::Begin("Temporaries"))
        {
            if (full)
            {
                ImGui::PushItemWidth(-1);

                static char filterBuf[64] {};
                ImGui::InputText("##Filter", filterBuf, sizeof(filterBuf));
                RenderLocals(filterBuf, asIDBLocalKey(selected_stack_entry, asIDBLocalType::Temporary));
                ImGui::PopItemWidth();
            }
        }
        ImGui::End();
        
        if (ImGui::Begin("Globals"))
        {
            if (full)
            {
                ImGui::PushItemWidth(-1);

                static char filterBuf[64] {};
                ImGui::InputText("##Filter", filterBuf, sizeof(filterBuf));
                RenderGlobals(filterBuf);
                ImGui::PopItemWidth();
            }
        }
        ImGui::End();
        
        if (ImGui::Begin("Watch"))
        {
            if (full)
            {
                ImGui::PushItemWidth(-1);
                RenderWatch();
                ImGui::PopItemWidth();
            }
        }
        ImGui::End();

        if (ImGui::Begin("Sections", nullptr, ImGuiWindowFlags_HorizontalScrollbar))
        {
            if (full)
            {
                for (auto &section : cache->sections)
                    ImGui::Selectable(section.second.data(), false);
            }
        }
        ImGui::End();
        
        if (ImGui::Begin("Source"))
        {
            if (full)
            {
                editor.Render("Source", ImVec2(-1, -1));
            }
        }
        ImGui::End();
    }
    
    ImGui::End();

    // Rendering
    ImGui::EndFrame();

    BackendRender();

    if (resetText)
        ChangeScript();
    else if (update_cursor)
    {
        if (!--update_cursor)
        {
            editor.SetCursor(update_row - 1, 0);
        }
    }
    
    auto mods = ImGui::GetIO().KeyMods;
    if (ImGui::IsKeyPressed(ImGuiKey::ImGuiKey_F5, false))
        debugger->Resume();
    else if (ImGui::IsKeyPressed(ImGuiKey::ImGuiKey_F10, false))
        debugger->StepOver();
    else if (ImGui::IsKeyPressed(ImGuiKey::ImGuiKey_F11, false) && (mods & ImGuiKey::ImGuiMod_Shift) == 0)
        debugger->StepInto();
    else if (ImGui::IsKeyPressed(ImGuiKey::ImGuiKey_F11, false) && (mods & ImGuiKey::ImGuiMod_Shift) == ImGuiKey::ImGuiMod_Shift)
        debugger->StepOut();
    else if (ImGui::IsKeyPressed(ImGuiKey::ImGuiKey_F9, false))
        editor.SetBreakpoint(editor.GetCursorLine(), debugger->ToggleBreakpoint(selected_stack_section, editor.GetCursorLine() + 1));

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
        if (var.value.expandable == asIDBExpandType::Children)
        {
            if (!var.queriedChildren)
                cache->QueryVariableChildren(varIt);
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
        else if (var.value.expandable == asIDBExpandType::Value)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const std::string_view s = var.value.value;
            ImGui::PushTextWrapPos(0);
            ImGui::TextUnformatted(s.data(), s.data() + s.size());
            ImGui::PopTextWrapPos();
        }
        ImGui::TreePop();
    }

    ImGui::PopID();
}