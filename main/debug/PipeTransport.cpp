#include "PipeTransport.h"

namespace sigflow {
namespace debug {

PipeTransport::PipeTransport(std::string pipeName, bool server)
    : pipe_(std::make_unique<sigflow::platform::LocalPipe>(std::move(pipeName), server))
{
}

PipeTransport::PipeTransport(std::unique_ptr<sigflow::platform::LocalPipe> pipe)
    : pipe_(std::move(pipe))
{
}

PipeTransport::~PipeTransport() = default;

bool PipeTransport::Open()
{
    return pipe_->Open();
}

void PipeTransport::Close()
{
    pipe_->Close();
}

bool PipeTransport::IsOpen() const
{
    return pipe_->IsOpen();
}

std::size_t PipeTransport::Read(std::uint8_t* data, std::size_t size, int timeoutMs)
{
    return pipe_->Read(data, size, timeoutMs);
}

bool PipeTransport::Write(const std::uint8_t* data, std::size_t size, int timeoutMs)
{
    return pipe_->Write(data, size, timeoutMs);
}

bool PipeTransport::CreatePair(std::string pipeName,
                               std::unique_ptr<PipeTransport>& hostEnd,
                               std::unique_ptr<PipeTransport>& deviceEnd,
                               std::string& error)
{
    std::unique_ptr<sigflow::platform::LocalPipe> hostPipe;
    std::unique_ptr<sigflow::platform::LocalPipe> devicePipe;
    if (!sigflow::platform::LocalPipe::CreatePair(std::move(pipeName), hostPipe,
                                                  devicePipe, error)) {
        return false;
    }

    hostEnd.reset(new PipeTransport(std::move(hostPipe)));
    deviceEnd.reset(new PipeTransport(std::move(devicePipe)));
    return true;
}

} // namespace debug
} // namespace sigflow
