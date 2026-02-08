#pragma once
#include <wx/wx.h>
#include "SigTree.h"

class SFNodePropertyPanel : public wxPanel {
    SigTreeNode* m_node;
    SFNodePropertyPanel();

    void loadNode(SigTreeNode* node);
};
