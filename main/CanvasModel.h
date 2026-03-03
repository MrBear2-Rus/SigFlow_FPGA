#pragma once
#include <wx/wx.h>
#include <vector>
#include <json/json.h>   

// 前向声明，避免循环包含
class SecondElement;

// 全局函数：读 JSON → 返回元件列表
std::vector<SecondElement> LoadSecondElements(const wxString& jsonPath);
