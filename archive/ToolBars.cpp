#include <wx/wx.h>
#include <wx/artprov.h>
#include <wx/toolbar.h>	
#include "ToolBars.h"
#include "MainFrame.h"
#include <wx/event.h>
#include "CanvasPanel.h"

ToolBars::ToolBars(MainFrame* owner)
    : m_owner(owner) {
    wxInitAllImageHandlers();
    //分配ID
    ArrangeIds(); // 分配 ID
    toolBar1 = CreateToolBar1(); // 创建工具栏 1
    toolBar2 = CreateToolBar2(); // 创建工具栏 2
    //toolBar3 = CreateToolBar3(); // 创建工具栏 3
}


// 分配工具栏工具的 ID 和路径
void ToolBars::ArrangeIds() {
    // 初始化工具栏 1 的 ID 和路径
    toolBar1_ids.resize(7); // 工具栏 1 有 7 个工具
    for (int i = 0; i < 7; ++i) {
        toolBar1_ids[i] = wxNewId();
    }
    toolBar1_toolPaths = {
        "res\\icons\\new.png",   "res\\icons\\open.png",  "res\\icons\\save.png",
        "res\\icons\\reclaim.png",   "res\\icons\\start.png",   "res\\icons\\stop.png",
        "res\\icons\\delete.png"
    };

    toolBar1_labels = {
        "新建文件", "打开文件", "保存文件", "撤回文件", "开始仿真",
        "终止仿真", "删除选中"
    };
    //实现ID对方法MAP
    toolIdToFunctionMap[toolBar1_ids[0]] = [this](int id) { OneChoose(id); m_owner->DoFileNew(); };
    toolIdToFunctionMap[toolBar1_ids[1]] = [this](int id) { OneChoose(id); m_owner->DoFileOpen(); };
    toolIdToFunctionMap[toolBar1_ids[2]] = [this](int id) { OneChoose(id); m_owner->DoFileSave(); };
    toolIdToFunctionMap[toolBar1_ids[3]] = [this](int id) { OneChoose(id); m_owner->DoEditUndo(); };
    toolIdToFunctionMap[toolBar1_ids[4]] = [this](int id) { OneChoose(id); m_owner->m_canvas->isSim = true; };
    toolIdToFunctionMap[toolBar1_ids[5]] = [this](int id) { OneChoose(id); m_owner->m_canvas->isSim = false; };
    toolIdToFunctionMap[toolBar1_ids[6]] = [this](int id) { OneChoose(id); m_owner->DoEditDelete();  };

    // 初始化工具栏 2 的 ID 和路径
    int size_2 = 5;
    toolBar2_ids.resize(size_2); // 工具栏 2 有 4 个工具
    for (int i = 0; i < size_2; ++i) {
        toolBar2_ids[i] = wxNewId();
    }
    toolBar2_toolPaths = {
        "res\\icons\\poke.png", "res\\icons\\select.png", "res\\icons\\eraser.png","res\\icons\\text.png", "res\\icons\\wiring.png", 
    };
    toolBar2_labels = {
        "拖动工具", "选中工具", "擦除工具","文本工具", "导线工具",
    };
    //实现ID对方法MAP
    toolIdToFunctionMap[toolBar2_ids[0]] = [this](int id) {OneChoose(id); m_owner->m_canvas->SetCurrentTool(ToolType::DRAG_TOOL); };
    toolIdToFunctionMap[toolBar2_ids[1]] = [this](int id) { OneChoose(id); m_owner->m_canvas->SetCurrentTool(ToolType::SELECT_TOOL); };
    toolIdToFunctionMap[toolBar2_ids[2]] = [this](int id) {OneChoose(id); m_owner->m_canvas->SetCurrentTool(ToolType::ERASER_TOOL); };
    toolIdToFunctionMap[toolBar2_ids[3]] = [this](int id) { OneChoose(id); m_owner->m_canvas->SetCurrentTool(ToolType::TEXT_TOOL); };
    toolIdToFunctionMap[toolBar2_ids[4]] = [this](int id) { OneChoose(id); m_owner->m_canvas->SetCurrentTool(ToolType::WIRE_TOOL); };

    // 初始化工具栏 3 的 ID 和路径
    toolBar3_ids.resize(8); // 工具栏 3 有 8 个工具
    for (int i = 0; i < 8; ++i) {
        toolBar3_ids[i] = wxNewId();
    }
    toolBar3_toolPaths = {
        "res\\tool_icons\\plus.png", "res\\tool_icons\\up.png", "res\\tool_icons\\down.png", "res\\tool_icons\\wrong.png",
        "res\\tool_icons\\start.png", "res\\tool_icons\\3.png", "res\\tool_icons\\2.png", "res\\tool_icons\\1.png"
    };
    toolBar3_labels = {
        "Plus", "Up", "Down", "Wrong", "1", "2", "3", "4"
    };
    toolIdToFunctionMap[toolBar3_ids[0]] = [this](int id) { OneChoose(id); };
    toolIdToFunctionMap[toolBar3_ids[1]] = [this](int id) { OneChoose(id); };
    toolIdToFunctionMap[toolBar3_ids[2]] = [this](int id) { OneChoose(id); };
    toolIdToFunctionMap[toolBar3_ids[3]] = [this](int id) { OneChoose(id); };
    toolIdToFunctionMap[toolBar3_ids[4]] = [this](int id) { OneChoose(id); };
    toolIdToFunctionMap[toolBar3_ids[5]] = [this](int id) { OneChoose(id); };
    toolIdToFunctionMap[toolBar3_ids[6]] = [this](int id) { OneChoose(id); };
    toolIdToFunctionMap[toolBar3_ids[7]] = [this](int id) { OneChoose(id); };
}



// 实现样式
wxToolBar* ToolBars::CreateToolBar1() {
    wxToolBar* toolbar = new wxToolBar(m_owner, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTB_FLAT);
    toolbar->SetToolBitmapSize(wxSize(24, 24)); // 设置图标大小
    //添加工具栏 1 的工具
    for (size_t i = 0; i < toolBar1_ids.size(); ++i) {
        wxBitmap bitmap(toolBar1_toolPaths[i], wxBITMAP_TYPE_PNG);
        //ShowTool(toolbar, toolBar1_ids[i], toolBar1_labels[i], bitmap);
        if (i==4 ||i==5) toolbar->AddCheckTool(toolBar1_ids[i], toolBar1_labels[i], bitmap);
        else toolbar->AddTool(toolBar1_ids[i], toolBar1_labels[i], bitmap);
        if (i == 2 || i == 3 || i == 5) toolbar->AddSeparator();
        toolbar->Bind(wxEVT_TOOL, &ToolBars::OnToolClicked, this, toolBar1_ids[i]);
    }

    return toolbar;
}

// 创建工具栏 2
wxToolBar* ToolBars::CreateToolBar2() {
    wxToolBar* toolbar = new wxToolBar(m_owner, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTB_FLAT);
    toolbar->SetToolBitmapSize(wxSize(24, 24)); // 设置图标大小
    wxInitAllImageHandlers();
    // 添加工具栏 2 的工具
    for (size_t i = 0; i < toolBar2_ids.size(); ++i) {
        wxBitmap bitmap(toolBar2_toolPaths[i], wxBITMAP_TYPE_PNG);
        //ShowTool(toolbar, toolBar2_ids[i], toolBar2_labels[i], wxBitmap(toolBar2_toolPaths[i], wxBITMAP_TYPE_PNG));
        toolbar->AddCheckTool(toolBar2_ids[i], toolBar2_labels[i], bitmap);
        toolbar->Bind(wxEVT_TOOL, &ToolBars::OnToolClicked, this, toolBar2_ids[i]);
    }

    return toolbar;
}

// 创建工具栏 3
wxToolBar* ToolBars::CreateToolBar3() {
    wxToolBar* toolbar = new wxToolBar(m_owner, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTB_FLAT);
    toolbar->SetToolBitmapSize(wxSize(24, 24)); // 设置图标大小

    // 添加工具栏 3 的工具
    wxInitAllImageHandlers();
    for (size_t i = 0; i < toolBar3_ids.size(); ++i) {
        wxBitmap bitmap(toolBar3_toolPaths[i], wxBITMAP_TYPE_PNG);
        //ShowTool(toolbar, toolBar3_ids[i], toolBar3_labels[i], bitmap);
        toolbar->AddCheckTool(toolBar3_ids[i], toolBar3_labels[i], bitmap);
        toolbar->Bind(wxEVT_TOOL, &ToolBars::OnToolClicked, this, toolBar3_ids[i]);
    }

    return toolbar;
}

ToolBars::~ToolBars() {
    // 析构函数的实现
    // 如果没有需要清理的资源，可以保持为空
}

void ToolBars::OnToolClicked(wxCommandEvent& event) {
    int toolId = event.GetId();

    // 安全检查：确保 m_owner 和工具管理器存在
    if (!m_owner) {
        wxLogError("ToolBars: m_owner is null!");
        return;
    }

    //CanvasEventHandler* CanvasEventHandler = m_owner->GetCanvasEventHandler();
    //if (!CanvasEventHandler) {
    //	wxLogError("ToolBars: CanvasEventHandler is null!");
    //	return;
    //}

    // 查找对应的工具 ID 并调用其方法
    auto it = toolIdToFunctionMap.find(toolId);
    if (it != toolIdToFunctionMap.end()) {
        it->second(toolId);
    }
}

void ToolBars::OneChoose(int toolId) {
    // 判断工具 ID 属于哪个工具栏，然后只取消该工具栏内其他工具的选择
    bool found = false;

    // 检查工具栏1
    for (size_t i = 0; i < toolBar1_ids.size(); ++i) {
        if (toolBar1_ids[i] == toolId) {
            found = true;
            // 只取消工具栏1内其他工具的选择
            for (size_t j = 0; j < toolBar1_ids.size(); ++j) {
                if (toolBar1_ids[j] != toolId) {
                    toolBar1->ToggleTool(toolBar1_ids[j], false);
                }
            }
            break;
        }
    }

    if (found) return; // 如果已经在工具栏1找到，直接返回

    // 检查工具栏2
    for (size_t i = 0; i < toolBar2_ids.size(); ++i) {
        if (toolBar2_ids[i] == toolId) {
            found = true;
            // 只取消工具栏2内其他工具的选择
            for (size_t j = 0; j < toolBar2_ids.size(); ++j) {
                if (toolBar2_ids[j] != toolId) {
                    toolBar2->ToggleTool(toolBar2_ids[j], false);
                }
            }
            break;
        }
    }

    if (found) return;

    // 检查工具栏3
    for (size_t i = 0; i < toolBar3_ids.size(); ++i) {
        if (toolBar3_ids[i] == toolId) {
            // 只取消工具栏3内其他工具的选择
            for (size_t j = 0; j < toolBar3_ids.size(); ++j) {
                if (toolBar3_ids[j] != toolId) {
                    toolBar3->ToggleTool(toolBar3_ids[j], false);
                }
            }
            break;
        }
    }
}

void ToolBars::ChoosePageOne_toolBar1(int toolId) {
    //工具栏3的第一页：toolBar1_ids[0] 到 toolBar3_ids[7]
    for (size_t i = 0; i < toolBar1_ids.size(); ++i) {
        // 如果是第一页的工具，则设置为启用状态
        HideTool(toolBar1, toolBar1_ids[i]);
        if (i <= 7) {
            ShowTool(toolBar1, toolBar1_ids[i], toolBar1_labels[i], wxBitmap(toolBar1_toolPaths[i], wxBITMAP_TYPE_PNG));
        }

        //HideTool(toolBar1, toolBar1_ids[0]);
        //HideTool(toolBar1, toolBar1_ids[1]);
        //HideTool(toolBar1, toolBar1_ids[2]);
        //HideTool(toolBar1, toolBar1_ids[3]);
    }
    OneChoose(toolBar2_ids[0]); // 默认选择第一个工具
}

void ToolBars::ChoosePageTwo_toolBar1(int toolId) {
    // 工具栏3的第二页：toolBar3_ids[7] 到 toolBar3_ids[15]
    for (size_t i = 0; i < toolBar1_ids.size(); ++i) {
        // 如果是第二页的工具，则设置为启用状态
        HideTool(toolBar1, toolBar1_ids[i]);
        if (i >= 5) {
            ShowTool(toolBar1, toolBar1_ids[i], toolBar1_labels[i], wxBitmap(toolBar1_toolPaths[i], wxBITMAP_TYPE_PNG));
        }
    }
    OneChoose(toolBar2_ids[1]);
}


void ToolBars::ChoosePageOne_toolBar3(int toolId) {
    // 工具栏3的第一页：toolBar3_ids[0] 到 toolBar3_ids[3]
    for (size_t i = 0; i < toolBar3_ids.size(); ++i) {
        // 如果是第一页的工具，则设置为启用状态
        HideTool(toolBar3, toolBar3_ids[i]);
        if (i <= 3) {
            ShowTool(toolBar3, toolBar3_ids[i], toolBar3_labels[i], wxBitmap(toolBar3_toolPaths[i], wxBITMAP_TYPE_PNG));
        }
    }
    OneChoose(toolBar2_ids[2]);
}

void ToolBars::ChoosePageTwo_toolBar3(int toolId) {
    // 工具栏3的第二页：toolBar3_ids[4] 到 toolBar3_ids[7]
    for (size_t i = 0; i < toolBar3_ids.size(); ++i) {
        // 如果是第二页的工具，则设置为启用状态
        HideTool(toolBar3, toolBar3_ids[i]);
        if (i >= 4) {
            ShowTool(toolBar3, toolBar3_ids[i], toolBar3_labels[i], wxBitmap(toolBar3_toolPaths[i], wxBITMAP_TYPE_PNG));
        }
    }
    OneChoose(toolBar2_ids[3]);
}


void ToolBars::HideTool(wxToolBar* toolbar, int toolId) {
    // 移除工具
    toolbar->DeleteTool(toolId);
    toolbar->Realize(); // 重新布局工具栏
}

void ToolBars::ShowTool(wxToolBar* toolbar, int toolId, const wxString& label, const wxBitmap& bitmap) {
    // 添加工具
    toolbar->AddCheckTool(toolId, label, bitmap);
    toolbar->Bind(wxEVT_TOOL, &ToolBars::OnToolClicked, this, toolId);
    toolbar->Realize(); // 重新布局工具栏
}
