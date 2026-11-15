#include "DurableFile.h"
#include <algorithm>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#endif
namespace eda::platform {
struct DurableFile::Impl {
#ifdef _WIN32
    HANDLE handle=INVALID_HANDLE_VALUE;
    ~Impl(){if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);}
#else
    int handle=-1;
    ~Impl(){if(handle>=0)close(handle);}
#endif
};
DurableFile::DurableFile(const std::filesystem::path& path,Mode mode):impl_(std::make_unique<Impl>()){
#ifdef _WIN32
    impl_->handle=CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ,nullptr,
        mode==Mode::CreateNew?CREATE_NEW:OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(impl_->handle==INVALID_HANDLE_VALUE)throw std::runtime_error("E_JOURNAL_OPEN_OR_BUSY: Win32 "+std::to_string(GetLastError()));
#else
    impl_->handle=open(path.c_str(),O_RDWR|(mode==Mode::CreateNew?O_CREAT|O_EXCL:0),0600);
    if(impl_->handle<0 || flock(impl_->handle,LOCK_EX|LOCK_NB))throw std::runtime_error("E_JOURNAL_OPEN_OR_BUSY");
#endif
}
DurableFile::~DurableFile()=default;
uint64_t DurableFile::Size()const{
#ifdef _WIN32
    LARGE_INTEGER size;if(!GetFileSizeEx(impl_->handle,&size))throw std::runtime_error("E_JOURNAL_IO");return uint64_t(size.QuadPart);
#else
    struct stat s;if(fstat(impl_->handle,&s))throw std::runtime_error("E_JOURNAL_IO");return uint64_t(s.st_size);
#endif
}
void DurableFile::Append(std::string_view data){
#ifdef _WIN32
    LARGE_INTEGER end{};if(!SetFilePointerEx(impl_->handle,end,nullptr,FILE_END))throw std::runtime_error("E_JOURNAL_IO");
#else
    if(lseek(impl_->handle,0,SEEK_END)<0)throw std::runtime_error("E_JOURNAL_IO");
#endif
    while(!data.empty()){
#ifdef _WIN32
        DWORD written=0;if(!WriteFile(impl_->handle,data.data(),DWORD(std::min<size_t>(data.size(),1u<<30)),&written,nullptr) || !written)
            throw std::runtime_error("E_JOURNAL_IO");
#else
        auto written=write(impl_->handle,data.data(),data.size());if(written<0 && errno==EINTR)continue;
        if(written<=0)throw std::runtime_error("E_JOURNAL_IO");
#endif
        data.remove_prefix(size_t(written));
    }
}
void DurableFile::Flush(){
#ifdef _WIN32
    if(!FlushFileBuffers(impl_->handle))throw std::runtime_error("E_JOURNAL_SYNC");
#else
    if(fsync(impl_->handle))throw std::runtime_error("E_JOURNAL_SYNC");
#endif
}
std::string DurableFile::ReadAll(size_t maximum){
    auto size=Size();if(size>maximum)throw std::runtime_error("E_JOURNAL_QUOTA");std::string data(size,'\0');
#ifdef _WIN32
    LARGE_INTEGER start{};if(!SetFilePointerEx(impl_->handle,start,nullptr,FILE_BEGIN))throw std::runtime_error("E_JOURNAL_IO");
#else
    if(lseek(impl_->handle,0,SEEK_SET)<0)throw std::runtime_error("E_JOURNAL_IO");
#endif
    size_t at=0;while(at<data.size()){
#ifdef _WIN32
        DWORD n=0;if(!ReadFile(impl_->handle,data.data()+at,DWORD(std::min<size_t>(data.size()-at,1u<<30)),&n,nullptr) || !n)
            throw std::runtime_error("E_JOURNAL_IO");
#else
        auto n=read(impl_->handle,data.data()+at,data.size()-at);if(n<0 && errno==EINTR)continue;if(n<=0)throw std::runtime_error("E_JOURNAL_IO");
#endif
        at+=size_t(n);
    }return data;
}
void DurableFile::Truncate(uint64_t size){
#ifdef _WIN32
    LARGE_INTEGER at;at.QuadPart=LONGLONG(size);if(!SetFilePointerEx(impl_->handle,at,nullptr,FILE_BEGIN) || !SetEndOfFile(impl_->handle))throw std::runtime_error("E_JOURNAL_IO");
#else
    if(ftruncate(impl_->handle,off_t(size)))throw std::runtime_error("E_JOURNAL_IO");
#endif
    Flush();
}
}
