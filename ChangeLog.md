# Yosys小组伍帅发现的问题和修改建议

## 一.新建.v文件时没有更新.project文件

### 问题

 新建.v文件时，.project的source_files数组不会更新这个文件，导致此文件没有创建对应的FileNode，因此在打开文件时,以下代码会传入空指针。

```C++
// MainFrame.cpp:1869-1876
void MainFrame::OnOpenFileFromTree(wxCommandEvent& evt) {
    wxString path = evt.GetString();
    FileNode* fn = sigTree->GetFileNode(path.ToStdString());  // 查找不到 FileNode
  
    m_verilogMgr->SetFileNode(fn, maps[path]);  // 传入空指针
}
```

于是在此处崩溃：

```C++
// VerilogManager.cpp:84-86
void VerilogManager::SetFileNode(FileNode* n, ...) {
    this->fn = n;                    // n 是 nullptr
    ProjectNode* pn = static_cast<ProjectNode*>(n->GetParent());
    //                                  
}
```

## 解决：

文件变更时实时更新.project，并重建SFTree。

此外，建议在选择菜单（ProjectStartManager）中，应该用选择.project文件而不是文件夹来打开项目，就像VisualStudio打开.sln一样
