#pragma once

#include <wx/wx.h>
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include "../jobs/PlatformProcess.h"

// 前向声明
class StimulusParser;
class TimelineGenerator;
class SimMainGenerator;

// 编译输出回调
using CompileOutputCallback = std::function<void(const wxString& line, bool isError)>;

// 仿真编译结果
struct SimulationCompileResult {
    bool success = false;
    wxString errorMessage;
    wxString dllPath;           // 生成的DLL路径
    wxString cacheDir;          // 缓存目录
};

// 仿真运行结果
struct SimulationRunResult {
    bool success = false;
    wxString errorMessage;
    wxString vcdPath;           // 生成的波形文件路径
};

// 编译进度回调
using CompileProgressCallback = std::function<void(int percent, const wxString& status)>;

class SimulationEngine
{
public:
    SimulationEngine();
    ~SimulationEngine();

    // 设置项目根目录（用于确定.sigflow缓存目录位置）
    void SetProjectRoot(const wxString& projectRoot);
    
    // 设置当前顶层模块名（跨会话运行仿真时需要）
    void SetTopModule(const wxString& topModule) { m_currentTopModule = topModule; }

    // Job 上下文：非空时进程经 JobService 登记并可被真取消。
    void SetJobContext(const wxString& projectPath, const wxString& jobId);

    // 是否有活跃的仿真作业（jobId 非空）
    bool HasActiveJob() const { std::lock_guard<std::mutex> lock(m_mutex); return !m_jobId.IsEmpty(); }

    wxString ActiveJobId() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_jobId;
    }

    // 设置编译进度回调
    void SetProgressCallback(CompileProgressCallback callback);

    // 编译仿真模型
    // topModule: 顶层模块名称（如 "fulladder"）
    // verilogFiles: 所有需要编译的Verilog文件路径列表
    SimulationCompileResult Compile(const wxString& topModule, 
                                    const std::vector<wxString>& verilogFiles);

    // 运行仿真（需要先有编译成功的DLL）
    // outputVcdPath: 输出的波形文件路径
    SimulationRunResult RunSimulation(const wxString& outputVcdPath);

    // 检查是否已经编译过（缓存是否有效）
    bool IsCompiled(const wxString& topModule) const;

    // 清除编译缓存
    bool CleanCache(const wxString& topModule);

    // 获取最后一次编译的信息
    const SimulationCompileResult& GetLastCompileResult() const { return m_lastResult; }

    // 获取最后一次编译的完整日志
    wxString GetLastCompileLog() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_lastCompileLog;
    }

    // 设置编译输出回调（实时接收编译输出）
    void SetCompileOutputCallback(CompileOutputCallback callback)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_outputCallback = callback;
    }

    // 检查是否正在编译中
    bool IsCompiling() const;

    // 取消当前编译
    void CancelCompile();

private:
    wxString m_projectRoot;         // 项目根目录
    wxString m_currentTopModule;    // 当前顶层模块名
    SimulationCompileResult m_lastResult;
    CompileProgressCallback m_progressCallback;
    CompileOutputCallback m_outputCallback;

    wxString m_lastCompileLog;      // 最后一次编译的完整日志
    bool m_isCompiling = false;     // 是否正在编译中
    bool m_dllCompileSuccess = false; // DLL编译是否成功
    mutable std::mutex m_mutex;     // 保护编译日志的互斥锁

    wxString m_jobProjectPath;
    wxString m_jobId;

    // 统一进程执行入口：PlatformProcess 是唯一触碰系统调用的地方。
    // streamOutput=true 时把输出增量写入 m_lastCompileLog 并转发 m_outputCallback。
    PlatformProcessResult RunTool(const wxString& executable,
                                  const std::vector<wxString>& arguments,
                                  const wxString& workingDirectory,
                                  int timeoutSeconds,
                                  bool streamOutput,
                                  bool rawCommandLine = false,
                                  const std::vector<std::pair<wxString, wxString>>& environment = {});

    // 获取缓存目录路径: <projectRoot>/.sigflow/sim/<topModule>/
    wxString GetCacheDirectory(const wxString& topModule) const;

    // 获取obj_dir路径
    wxString GetObjDirPath(const wxString& topModule) const;

    // 执行Verilator生成C++代码
    bool RunVerilator(const wxString& topModule, 
                      const std::vector<wxString>& verilogFiles,
                      wxString& errorMsg);

    // 编译生成DLL
    bool CompileToDll(const wxString& topModule, wxString& errorMsg);

    // 创建目录（如果不存在）
    bool CreateDirectoryRecursive(const wxString& path);
    
    // 创建 sc_time_stub.cpp 文件
    void CreateScTimeStub(const wxString& path);

    // 查找Verilator安装路径
    wxString FindVerilatorPath() const;

    // 查找 Verilator C++ 运行时头文件和实现目录
    wxString FindVerilatorIncludePath() const;

    // 获取软件自身所在目录（用于找到 sc_time_stub.cpp 等工具文件）
    wxString GetSoftwareDirectory() const;

    // 进度报告辅助函数
    void ReportProgress(int percent, const wxString& status);

    // 在项目 src 目录下模糊匹配 Testbench 文件
    wxString FindTestbenchFile(const wxString& projectRoot) const;

    // 生成 sim_main.cpp 并编译为 sim_runner.exe
    bool CompileSimRunner(const wxString& topModule, const wxString& testbenchPath,
                          wxString& errorMsg);

    // 运行 sim_runner.exe 生成 VCD
    bool ExecuteSimRunner(const wxString& topModule, wxString& errorMsg);
};
