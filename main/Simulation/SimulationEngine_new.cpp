// 这个函数创建 sc_time_stub.cpp 文件
void SimulationEngine::CreateScTimeStub(const wxString& path)
{
    const char* stubContent = 
        "// Stub for sc_time_stamp function required by Verilator\\n"
        "#include <cstdint>\\n"
        "\\n"
        "static uint64_t g_sim_time = 0;\\n"
        "\\n"
        "double sc_time_stamp() {\\n"
        "    return static_cast<double>(g_sim_time);\\n"
        "}\\n"
        "\\n"
        "void advance_sim_time(uint64_t delta) {\\n"
        "    g_sim_time += delta;\\n"
        "}\\n";
    
    wxFile file(path, wxFile::write);
    if (file.IsOpened()) {
        file.Write(wxString::FromUTF8(stubContent));
        file.Close();
    }
}
